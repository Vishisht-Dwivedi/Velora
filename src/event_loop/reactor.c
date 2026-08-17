#include "velora/reactor.h"
volatile sig_atomic_t running_status = RUNNING;

vr_result_t vr_reactor_create(vr_reactor_t *reactor)
{
    if (reactor == NULL)
    {
        vr_log(VR_LOG_ERROR, "NULL reactor passed");
        return VR_ERROR;
    }
    int e_fd = epoll_create1(EPOLL_CLOEXEC);
    if(e_fd == -1)
    {
        if(errno == EINTR)
        {
            vr_log(VR_LOG_INFO, "Shutdown called");
            return VR_INTERRUPTED;
        }
        vr_perror("Error while creating epoll file descriptor");
        return VR_ERROR;
    }
    reactor->epoll_fd = e_fd;
    vr_log(VR_LOG_INFO, "epoll fd created: %d", e_fd);
    return VR_SUCCESS;
}

vr_result_t vr_reactor_destroy(vr_reactor_t *reactor)
{
    if (reactor == NULL)
    {
        vr_log(VR_LOG_ERROR, "NULL reactor passed");
        return VR_ERROR;
    }
    if(close(reactor->epoll_fd) == -1)
    {
        vr_perror("Error on closing fd for epoll");
        return VR_ERROR;
    }
    reactor->epoll_fd = -1;
    vr_log(VR_LOG_INFO, "Closed epoll file descriptor");
    return VR_SUCCESS;
}

vr_result_t vr_reactor_add(vr_reactor_t *reactor, vr_connection_t *conn, u_int32_t events)
{
    if (reactor == NULL)
    {
        vr_log(VR_LOG_ERROR, "NULL reactor passed");
        return VR_ERROR;
    }
    struct epoll_event ev = {0};
    ev.events = events;
    ev.data.ptr = conn;
    int res = epoll_ctl(reactor->epoll_fd, EPOLL_CTL_ADD, conn->net_conn.fd, &ev);
    if(res == -1)
    {
        vr_perror("Error on adding fd to reactor events");
        return VR_ERROR;
    }
    vr_log(VR_LOG_INFO, "Added element to reactor loop successfully");
    return VR_SUCCESS;
}

vr_result_t vr_reactor_wait(vr_reactor_t *reactor, int timeout)
{
    if (reactor == NULL)
    {
        vr_log(VR_LOG_ERROR, "NULL reactor passed");
        return VR_ERROR;
    }
    int res = epoll_wait(reactor->epoll_fd, reactor->events, VR_REACTOR_MAX_EVENTS, timeout);
    if(res == -1)
    {
        if (errno == EINTR)
        {
            vr_log(VR_LOG_INFO, "Closing epoll");
            return VR_ERROR;
        }
        vr_perror("Error while waiting on epoll");
        return VR_ERROR;
    }
    reactor->ready_events = res;
    vr_log(VR_LOG_INFO, "Retrieved %d events", res);
    return VR_SUCCESS;
}

vr_result_t vr_reactor_remove(vr_reactor_t *reactor, vr_connection_t *conn)
{
    if (reactor == NULL)
    {
        vr_log(VR_LOG_ERROR, "NULL reactor passed");
        return VR_ERROR;
    }
    int res = epoll_ctl(reactor->epoll_fd, EPOLL_CTL_DEL, conn->net_conn.fd, NULL);
    if(res == -1)
    {
        vr_perror("Error on deleting fd from reactor events");
        return VR_ERROR;
    }
    vr_log(VR_LOG_INFO, "Removed element from reactor loop successfully");
    return VR_SUCCESS;    
}

vr_result_t vr_reactor_loop(vr_reactor_t *reactor, vr_connection_manager_t *manager, int port)
{
    int listen_fd = 0;
    reactor->epoll_fd = -1;
    if (vr_reactor_create(reactor) != VR_SUCCESS)
    {
        vr_log(VR_LOG_ERROR, "Failed to create reactor, aborting startup");
        return VR_ERROR;
    }
    if (vr_tcp_server_create(port, &listen_fd) != VR_SUCCESS)
    {
        vr_log(VR_LOG_ERROR, "Failed to create TCP server on port: %d", port);
        vr_reactor_destroy(reactor);
        return VR_ERROR;
    }
    vr_log(VR_LOG_INFO, "Server Started on port: %d", port);
    if (vr_socket_set_non_blocking(listen_fd) != VR_SUCCESS)
    {
        vr_log(VR_LOG_ERROR, "Failed to set listening socket to non blocking");
        close(listen_fd);
        vr_reactor_destroy(reactor);
        return VR_ERROR;
    }
    vr_log(VR_LOG_INFO, "Set listening socket to non blocking");
    vr_connection_t *listener_conn = malloc(sizeof(vr_connection_t));
    if(listener_conn == NULL)
    {
        vr_perror("Malloc failed while allocating mem for listener connection in reactor loop");
        close(listen_fd);
        vr_reactor_destroy(reactor);
        return VR_ERROR;
    }
    memset(listener_conn, 0, sizeof(*listener_conn));
    listener_conn->net_conn.fd = listen_fd;
    listener_conn->type = VR_CONN_LISTENER;
    if (vr_reactor_add(reactor, listener_conn, EPOLLIN | EPOLLET) != VR_SUCCESS)
    {
        vr_log(VR_LOG_ERROR, "Failed to register listener socket with reactor");
        close(listen_fd);
        free(listener_conn);
        vr_reactor_destroy(reactor);
        return VR_ERROR;
    }

    while (running_status == RUNNING)
    {
        if (vr_reactor_wait(reactor, -1) != VR_SUCCESS)
            break;
        for (int i = 0; i < reactor->ready_events; i++)
        {
            vr_connection_t *ready_conn = reactor->events[i].data.ptr;
            int ready_fd = ready_conn->net_conn.fd;
            vr_log(VR_LOG_INFO, "Ready fd: %d", ready_fd);
            switch (ready_conn->type)
            {
                case VR_CONN_LISTENER: {
                    //if socket is a listener.. all it does is accept.. create.. and add...
                    while (true)
                    {
                        vr_net_conn_t new_net_conn = {0};
                        vr_result_t res = vr_tcp_accept(ready_fd, &new_net_conn);
                        if (res == VR_SUCCESS)
                        {
                            vr_connection_t *new_conn = vr_connection_create(manager, new_net_conn);
                            if(new_conn == NULL)
                            {
                                close(new_net_conn.fd);
                                continue;
                            }
                            new_conn->type = VR_CONN_CLIENT;
                            vr_socket_set_non_blocking(new_conn->net_conn.fd);
                            if (vr_reactor_add(reactor, new_conn, EPOLLIN | EPOLLET) != VR_SUCCESS)
                            {
                                close(new_conn->net_conn.fd);
                                vr_connection_destroy(manager, new_conn);
                            }
                            continue;
                        }
                        if (res == VR_EMPTY) break;
                        if (res == VR_INTERRUPTED)
                        {
                            vr_log(VR_LOG_INFO, "Stopped calling accept");
                            break;
                        }
                        vr_perror("Error in socket accept");
                        break;
                    }
                    break;
                }
                case VR_CONN_CLIENT: {
                    bool disconnected = false;
                    vr_packet_t out = {0};
                    uint32_t events = reactor->events[i].events;
                    if (events & EPOLLOUT)
                    {
                        while (!vr_conn_ring_buf_empty(&ready_conn->write_buf))
                        {
                            uint8_t *data = NULL;
                            uint32_t available = vr_conn_ring_buf_contiguous_read(&ready_conn->write_buf, &data);
                            if (available == 0)
                                break;
                            ssize_t sent = vr_socket_send(ready_fd, data, available, MSG_NOSIGNAL);
                            if (sent > 0)
                            {
                                vr_conn_ring_buf_consume(&ready_conn->write_buf, (uint32_t)sent);
                                continue;
                            }
                            if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                                break;
                            vr_log(VR_LOG_ERROR, "Socket write failed for fd=%d", ready_fd);
                            vr_reactor_remove(reactor, ready_conn);
                            close(ready_fd);
                            vr_connection_destroy(manager, ready_conn);
                            disconnected = true;
                            break;
                        }
                        if (!disconnected && vr_conn_ring_buf_empty(&ready_conn->write_buf))
                            vr_reactor_modify(reactor, ready_conn, EPOLLIN | EPOLLET);
                    }
                    if (!disconnected && (events & EPOLLIN))
                    {
                        while (true)
                    {
                        ssize_t len = vr_socket_recv_ring_buf(ready_fd, &(ready_conn->read_buf), 0);
                        if (len == 0)
                        {
                            vr_log(VR_LOG_INFO, "Client Disconnected");
                            vr_reactor_remove(reactor, ready_conn);
                            close(ready_fd);
                            if (vr_connection_destroy(manager, ready_conn) == VR_ERROR)
                            {
                                vr_log(VR_LOG_ERROR, "Failed to clean up connection state for fd=%d", ready_fd);
                            }
                            disconnected = true;
                            break;
                        }
                        else if (len == -1)
                        {
                            if (errno == EAGAIN || errno == EWOULDBLOCK)
                            {
                                break;
                            }
                            else if (errno == ENOBUFS)
                            {
                                if (ready_conn->read_buf.state == VR_CONN_RING_BUF_UNALLOC) 
                                {
                                    if (vr_conn_ring_buf_init(&(ready_conn->read_buf)) == VR_ERROR)
                                        break;
                                    vr_log(VR_LOG_INFO, "Ring buffer allocated cap=%u", ready_conn->read_buf.capacity);
                                }
                                else
                                {
                                    if (vr_conn_ring_buf_grow(&(ready_conn->read_buf)) == VR_ERROR)
                                        break;
                                    vr_log(VR_LOG_INFO, "Growing ring buffer: old_cap=%u count=%u read=%u write=%u", ready_conn->read_buf.capacity, ready_conn->read_buf.count, ready_conn->read_buf.read_pos, ready_conn->read_buf.write_pos);
                                    continue;
                                }
                            }
                            else
                            {
                                vr_reactor_remove(reactor, ready_conn);
                                close(ready_fd);
                                if (vr_connection_destroy(manager, ready_conn) == VR_ERROR)
                                {
                                    vr_log(VR_LOG_ERROR, "Failed to clean up connection state for fd=%d", ready_fd);
                                }
                                disconnected = true;
                                break;
                            }
                        }
                    }
                    if(!disconnected)
                    {
                        vr_parser_poll(&(ready_conn->parser), ready_conn, &out);
                        vr_packet_t *response = vr_protocol_handle_packet(ready_conn, &out);
                        if (response != NULL)
                        {
                            size_t packet_size = sizeof(vr_packet_header_t) + response->header.payload_len;
                            uint8_t *serialized = malloc(packet_size);
                            if (serialized == NULL)
                            {
                                vr_log(VR_LOG_ERROR, "Failed to allocate response buffer for serialization");
                                free(response->payload);
                                free(response);
                                break;
                            }
                            vr_packet_serialize(response, serialized);
                            bool write_buf_failed = false;
                            for (size_t j = 0; j < packet_size && !write_buf_failed; j++)
                            {
                                while (vr_conn_ring_buf_push(&ready_conn->write_buf, serialized[j]) == VR_ERROR)
                                {
                                    if (ready_conn->write_buf.state == VR_CONN_RING_BUF_UNALLOC)
                                    {
                                        if (vr_conn_ring_buf_init(&ready_conn->write_buf) == VR_ERROR)
                                        {
                                            write_buf_failed = true;
                                            break;
                                        }
                                    } else if (ready_conn->write_buf.state == VR_CONN_RING_BUF_FULL){
                                        if (vr_conn_ring_buf_grow(&ready_conn->write_buf) == VR_ERROR)
                                        {
                                            write_buf_failed = true;
                                            break;
                                        }
                                    }
                                }
                            }
                            free(serialized);
                            free(response->payload);
                            free(response);
                            if (write_buf_failed)
                                vr_log(VR_LOG_ERROR, "Failed to buffer response for fd=%d", ready_fd);
                            else
                                vr_reactor_modify(reactor, ready_conn, EPOLLIN | EPOLLOUT | EPOLLET);
                        }
                    }
                    break;
                }
                }
                    
            }
        }
        reactor->ready_events = 0;
    }
    while(manager->count > 0)
    {
        vr_connection_destroy(manager, manager->slots[0]);
    }
    vr_reactor_remove(reactor, listener_conn);
    close(listen_fd);
    free(listener_conn);
    vr_reactor_destroy(reactor);
    return VR_SUCCESS;
}
vr_result_t vr_reactor_modify(vr_reactor_t *reactor, vr_connection_t *conn, uint32_t events)
{
    if (reactor == NULL || conn == NULL)
        return VR_ERROR;

    struct epoll_event ev = {0};
    ev.events = events;
    ev.data.ptr = conn;
    if (epoll_ctl(reactor->epoll_fd, EPOLL_CTL_MOD, conn->net_conn.fd, &ev) == -1)
    {
        vr_perror("Error modifying reactor events");
        return VR_ERROR;
    }
    return VR_SUCCESS;
}