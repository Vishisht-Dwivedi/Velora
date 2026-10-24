#include "velora/mtcp_runtime.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>

#include <mtcp_epoll.h>
#include <sys/socket.h>

#define VR_MTCP_EVENT_BATCH_SIZE 64

static vr_connection_t *find_connection(vr_mtcp_runtime_t *runtime,
                                        int socket_id)
{
    for (size_t index = 0; index < runtime->connections.count; index++) {
        vr_connection_t *connection = runtime->connections.slots[index];
        if (connection->net_conn.fd == socket_id) {
            return connection;
        }
    }
    return NULL;
}

static int close_connection(vr_mtcp_runtime_t *runtime,
                            vr_connection_t *connection)
{
    struct mtcp_epoll_event event = { 0 };
    int result = 0;

    vr_log_component(VR_COMPONENT_TCP,
                     VR_LOG_INFO,
                     "closing socket=%d",
                     connection->net_conn.fd);
    event.data.sockid = connection->net_conn.fd;
    if (mtcp_epoll_ctl(runtime->context, runtime->epoll_fd,
                       MTCP_EPOLL_CTL_DEL, connection->net_conn.fd,
                       &event) < 0) {
        result = -1;
    }
    if (mtcp_close(runtime->context, connection->net_conn.fd) < 0) {
        result = -1;
    }
    if (vr_connection_destroy(&runtime->connections, connection) == VR_ERROR) {
        result = -1;
    }
    return result;
}

static int set_write_interest(vr_mtcp_runtime_t *runtime,
                              vr_connection_t *connection,
                              bool enabled)
{
    struct mtcp_epoll_event event = { 0 };

    event.events = MTCP_EPOLLIN | MTCP_EPOLLET;
    if (enabled) {
        event.events |= MTCP_EPOLLOUT;
    }
    event.data.sockid = connection->net_conn.fd;
    return mtcp_epoll_ctl(runtime->context, runtime->epoll_fd,
                          MTCP_EPOLL_CTL_MOD, connection->net_conn.fd, &event);
}

static int queue_response(vr_connection_t *connection, vr_packet_t *response)
{
    size_t packet_size = VR_HDR_SIZE + response->header.payload_len;
    uint8_t *serialized;
    size_t offset = 0;

    vr_log_component(VR_COMPONENT_PROTOCOL,
                     VR_LOG_INFO,
                     "queueing response type=%u payload_len=%u socket=%d",
                     response->header.type,
                     response->header.payload_len,
                     connection->net_conn.fd);
    if (response->header.payload_len > 0 && response->payload == NULL) {
        errno = EINVAL;
        goto fail;
    }

    serialized = malloc(packet_size);
    if (serialized == NULL) {
        goto fail;
    }
    vr_packet_serialize(response, serialized);

    if (vr_conn_ring_buf_reserve(&connection->write_buf,
                                 (uint32_t)packet_size) == VR_ERROR) {
        free(serialized);
        goto fail;
    }

    while (offset < packet_size) {
        uint8_t *region = NULL;
        uint32_t region_size = vr_conn_ring_buf_contiguous_write(
            &connection->write_buf, &region);
        size_t remaining = packet_size - offset;
        size_t chunk = region_size < remaining ? region_size : remaining;

        if (region == NULL || chunk == 0) {
            free(serialized);
            errno = EIO;
            goto fail;
        }
        memcpy(region, serialized + offset, chunk);
        if (vr_conn_ring_buf_commit(&connection->write_buf,
                                    (uint32_t)chunk) == VR_ERROR) {
            free(serialized);
            errno = EIO;
            goto fail;
        }
        offset += chunk;
    }

    free(serialized);
    free(response->payload);
    free(response);
    return 0;

fail:
    free(response->payload);
    free(response);
    return -1;
}

static int flush_connection_output(vr_mtcp_runtime_t *runtime,
                                   vr_connection_t *connection,
                                   size_t *bytes_accepted_for_send)
{
    while (!vr_conn_ring_buf_empty(&connection->write_buf)) {
        uint8_t *data = NULL;
        uint32_t available = vr_conn_ring_buf_contiguous_read(
            &connection->write_buf, &data);
        ssize_t sent;

        if (available == 0 || data == NULL) {
            errno = EIO;
            return -1;
        }

        sent = mtcp_write(runtime->context, connection->net_conn.fd,
                          (const char *)data, available);
        if (sent > 0) {
            if ((size_t)sent > available ||
                vr_conn_ring_buf_consume(&connection->write_buf,
                                         (uint32_t)sent) == VR_ERROR) {
                errno = EIO;
                return -1;
            }
            *bytes_accepted_for_send += (size_t)sent;
            continue;
        }

        if (sent == 0 || errno == EAGAIN || errno == EWOULDBLOCK ||
            errno == EINTR) {
            return 0;
        }
        return -1;
    }

    return 1;
}

static int process_connection_data(vr_mtcp_runtime_t *runtime,
                                   vr_connection_t *connection,
                                   size_t *bytes_received,
                                   size_t *packets_processed)
{
    for (;;) {
        uint8_t *write_region = NULL;
        uint32_t write_capacity;
        ssize_t received;

        if (vr_conn_ring_buf_reserve(&connection->read_buf, 1) == VR_ERROR) {
            return -1;
        }
        write_capacity = vr_conn_ring_buf_contiguous_write(
            &connection->read_buf, &write_region);
        if (write_capacity == 0) {
            errno = ENOBUFS;
            return -1;
        }

        received = mtcp_recv(runtime->context, connection->net_conn.fd,
                             (char *)write_region, write_capacity, 0);
        if (received > 0) {
            vr_log_component(VR_COMPONENT_TCP,
                             VR_LOG_INFO,
                             "socket=%d recv_bytes=%zd write_capacity=%u",
                             connection->net_conn.fd,
                             (ssize_t)received,
                             write_capacity);
            if (vr_conn_ring_buf_commit(&connection->read_buf,
                                        (uint32_t)received) == VR_ERROR) {
                errno = EIO;
                return -1;
            }
            *bytes_received += (size_t)received;

            for (;;) {
                vr_packet_t packet = { 0 };
                vr_result_t parsed = vr_parser_poll(&connection->parser,
                                                    connection, &packet);
                if (parsed == VR_EMPTY) {
                    break;
                }
                if (parsed == VR_ERROR) {
                    vr_log_component(VR_COMPONENT_PROTOCOL,
                                     VR_LOG_ERROR,
                                     "socket=%d parser rejected packet",
                                     connection->net_conn.fd);
                    free(packet.payload);
                    return -1;
                }

                vr_log_component(VR_COMPONENT_PROTOCOL,
                                 VR_LOG_INFO,
                                 "socket=%d parsed packet type=%u payload_len=%u",
                                 connection->net_conn.fd,
                                 packet.header.type,
                                 packet.header.payload_len);
                vr_packet_t *response = vr_protocol_handle_packet(connection,
                                                                  &packet);
                free(packet.payload);
                if (response != NULL) {
                    bool output_was_empty =
                        vr_conn_ring_buf_empty(&connection->write_buf);
                    if (queue_response(connection, response) < 0) {
                        return -1;
                    }
                    if (output_was_empty &&
                        set_write_interest(runtime, connection, true) < 0) {
                        return -1;
                    }
                }
                (*packets_processed)++;
            }
            continue;
        }

        if (received == 0) {
            return 1;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        if (errno == EINTR) {
            return 0;
        }
        return -1;
    }
}

int vr_mtcp_runtime_destroy(vr_mtcp_runtime_t *runtime)
{
    int result = 0;

    if (runtime == NULL) {
        errno = EINVAL;
        return -1;
    }

    vr_log_component(VR_COMPONENT_BUILD,
                     VR_LOG_INFO,
                     "destroying mTCP runtime context=%p",
                     (void *)runtime->context);
    if (runtime->context != NULL) {
        while (runtime->connections.count > 0) {
            vr_connection_t *connection =
                runtime->connections.slots[runtime->connections.count - 1];
            if (mtcp_close(runtime->context, connection->net_conn.fd) < 0) {
                result = -1;
            }
            if (vr_connection_destroy(&runtime->connections, connection) == VR_ERROR) {
                result = -1;
            }
        }

        if (runtime->listener_socket >= 0 &&
            mtcp_close(runtime->context, runtime->listener_socket) < 0) {
            result = -1;
        }
        runtime->listener_socket = -1;

        if (runtime->epoll_fd >= 0 &&
            mtcp_close(runtime->context, runtime->epoll_fd) < 0) {
            result = -1;
        }
    }

    if (runtime->connections_initialized) {
        if (vr_connection_manager_destroy(&runtime->connections) == VR_ERROR) {
            result = -1;
        }
        runtime->connections_initialized = 0;
    }
    runtime->epoll_fd = -1;

    if (runtime->context != NULL) {
        mtcp_destroy_context(runtime->context);
        runtime->context = NULL;
    }

    if (runtime->global_initialized) {
        mtcp_destroy();
        runtime->global_initialized = 0;
    }

    return result;
}

int vr_mtcp_runtime_init(vr_mtcp_runtime_t *runtime, const char *config_file)
{
    if (runtime == NULL || config_file == NULL || config_file[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (runtime->context != NULL || runtime->epoll_fd >= 0 ||
        runtime->listener_socket >= 0 || runtime->global_initialized ||
        runtime->connections_initialized) {
        errno = EALREADY;
        return -1;
    }

    vr_log_component(VR_COMPONENT_BUILD,
                     VR_LOG_INFO,
                     "initializing mTCP runtime config=%s",
                     config_file);
    if (vr_connection_manager_init(&runtime->connections, 16) == VR_ERROR) {
        return -1;
    }
    runtime->connections_initialized = 1;

    if (mtcp_init(config_file) != 0) {
        vr_log_component(VR_COMPONENT_BUILD,
                         VR_LOG_ERROR,
                         "mtcp_init failed config=%s errno=%d",
                         config_file,
                         errno);
        goto fail;
    }
    runtime->global_initialized = 1;

    runtime->context = mtcp_create_context(0);
    if (runtime->context == NULL) {
        vr_log_component(VR_COMPONENT_BUILD,
                         VR_LOG_ERROR,
                         "mtcp_create_context failed config=%s",
                         config_file);
        goto fail;
    }

    runtime->epoll_fd = mtcp_epoll_create(runtime->context,
                                          VR_MTCP_EVENT_BATCH_SIZE);
    if (runtime->epoll_fd < 0) {
        vr_log_component(VR_COMPONENT_BUILD,
                         VR_LOG_ERROR,
                         "mtcp_epoll_create failed errno=%d",
                         errno);
        goto fail;
    }

    vr_log_component(VR_COMPONENT_BUILD,
                     VR_LOG_INFO,
                     "mTCP runtime ready context=%p epoll_fd=%d",
                     (void *)runtime->context,
                     runtime->epoll_fd);
    return 0;

fail:
    vr_mtcp_runtime_destroy(runtime);
    return -1;
}

int vr_mtcp_runtime_listen(vr_mtcp_runtime_t *runtime,
                           const char *ipv4_address,
                           uint16_t port,
                           int backlog)
{
    struct sockaddr_in address = { 0 };

    if (runtime == NULL || runtime->context == NULL || runtime->epoll_fd < 0 ||
        ipv4_address == NULL || port == 0 || backlog <= 0 ||
        runtime->listener_socket >= 0) {
        errno = EINVAL;
        return -1;
    }

    vr_log_component(VR_COMPONENT_TCP,
                     VR_LOG_INFO,
                     "binding listener addr=%s port=%u backlog=%d",
                     ipv4_address,
                     (unsigned int)port,
                     backlog);
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, ipv4_address, &address.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }

    runtime->listener_socket = mtcp_socket(runtime->context, AF_INET,
                                            SOCK_STREAM, 0);
    if (runtime->listener_socket < 0 ||
        mtcp_setsock_nonblock(runtime->context, runtime->listener_socket) < 0 ||
        mtcp_bind(runtime->context, runtime->listener_socket,
                  (const struct sockaddr *)&address, sizeof(address)) < 0 ||
        mtcp_listen(runtime->context, runtime->listener_socket, backlog) < 0) {
        vr_log_component(VR_COMPONENT_TCP,
                         VR_LOG_ERROR,
                         "listener setup failed addr=%s port=%u errno=%d",
                         ipv4_address,
                         (unsigned int)port,
                         errno);
        return -1;
    }

    vr_log_component(VR_COMPONENT_TCP,
                     VR_LOG_INFO,
                     "listener ready socket=%d addr=%s port=%u",
                     runtime->listener_socket,
                     ipv4_address,
                     (unsigned int)port);
    return 0;
}

int vr_mtcp_runtime_accept_one(vr_mtcp_runtime_t *runtime,
                               int *accepted_socket)
{
    int socket_id;

    if (runtime == NULL || runtime->context == NULL || runtime->epoll_fd < 0 ||
        runtime->listener_socket < 0 || accepted_socket == NULL) {
        errno = EINVAL;
        return -1;
    }

    *accepted_socket = -1;
    socket_id = mtcp_accept(runtime->context, runtime->listener_socket,
                            NULL, NULL);
    if (socket_id < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        vr_log_component(VR_COMPONENT_TCP,
                         VR_LOG_ERROR,
                         "accept failed listener_socket=%d errno=%d",
                         runtime->listener_socket,
                         errno);
        return -1;
    }

    vr_log_component(VR_COMPONENT_TCP,
                     VR_LOG_INFO,
                     "accepted socket=%d",
                     socket_id);
    if (mtcp_setsock_nonblock(runtime->context, socket_id) < 0) {
        int saved_errno = errno;
        mtcp_close(runtime->context, socket_id);
        errno = saved_errno;
        return -1;
    }

    struct mtcp_epoll_event event = { 0 };
    event.events = MTCP_EPOLLIN | MTCP_EPOLLET;
    event.data.sockid = socket_id;
    if (mtcp_epoll_ctl(runtime->context, runtime->epoll_fd,
                       MTCP_EPOLL_CTL_ADD, socket_id, &event) < 0) {
        int saved_errno = errno;
        mtcp_close(runtime->context, socket_id);
        errno = saved_errno;
        return -1;
    }

    vr_net_conn_t net_connection = { .fd = socket_id };
    vr_connection_t *connection = vr_connection_create(
        &runtime->connections, net_connection);
    if (connection == NULL) {
        struct mtcp_epoll_event delete_event = { 0 };
        delete_event.data.sockid = socket_id;
        mtcp_epoll_ctl(runtime->context, runtime->epoll_fd,
                       MTCP_EPOLL_CTL_DEL, socket_id, &delete_event);
        int saved_errno = errno;
        mtcp_close(runtime->context, socket_id);
        errno = saved_errno;
        return -1;
    }
    connection->type = VR_CONN_CLIENT;

    *accepted_socket = socket_id;
    return 1;
}

int vr_mtcp_runtime_process_events(vr_mtcp_runtime_t *runtime,
                                   int timeout_ms,
                                   size_t *bytes_received,
                                   size_t *packets_processed,
                                   size_t *bytes_accepted_for_send)
{
    struct mtcp_epoll_event events[VR_MTCP_EVENT_BATCH_SIZE];
    int event_count;

    if (runtime == NULL || runtime->context == NULL || runtime->epoll_fd < 0 ||
        bytes_received == NULL || packets_processed == NULL ||
        bytes_accepted_for_send == NULL || timeout_ms < -1) {
        errno = EINVAL;
        return -1;
    }

    *bytes_received = 0;
    *packets_processed = 0;
    *bytes_accepted_for_send = 0;
    event_count = mtcp_epoll_wait(runtime->context, runtime->epoll_fd, events,
                                  VR_MTCP_EVENT_BATCH_SIZE, timeout_ms);
    if (event_count < 0) {
        vr_log_component(VR_COMPONENT_TCP,
                         VR_LOG_ERROR,
                         "epoll_wait failed timeout_ms=%d errno=%d",
                         timeout_ms,
                         errno);
        return errno == EINTR ? 0 : -1;
    }

    vr_log_component(VR_COMPONENT_TCP,
                     VR_LOG_INFO,
                     "epoll_wait returned %d events timeout_ms=%d",
                     event_count,
                     timeout_ms);
    for (int index = 0; index < event_count; index++) {
        uint32_t close_events = MTCP_EPOLLERR | MTCP_EPOLLHUP | MTCP_EPOLLRDHUP;
        vr_connection_t *connection = find_connection(
            runtime, events[index].data.sockid);
        int receive_result = 0;

        if (connection == NULL) {
            continue;
        }

        if (events[index].events & (MTCP_EPOLLIN | close_events)) {
            receive_result = process_connection_data(runtime, connection,
                                                     bytes_received,
                                                     packets_processed);
        }

        if (receive_result != 0 || (events[index].events & close_events)) {
            close_connection(runtime, connection);
            continue;
        }

        if (events[index].events & MTCP_EPOLLOUT) {
            int write_result = flush_connection_output(
                runtime, connection, bytes_accepted_for_send);
            if (write_result < 0) {
                close_connection(runtime, connection);
                continue;
            }
            if (write_result > 0 &&
                set_write_interest(runtime, connection, false) < 0) {
                close_connection(runtime, connection);
            }
        }
    }

    return 0;
}