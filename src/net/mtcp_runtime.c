#include "velora/mtcp_runtime.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>

#include <mtcp_epoll.h>
#include <sys/socket.h>

int vr_mtcp_runtime_destroy(vr_mtcp_runtime_t *runtime)
{
    int result = 0;

    if (runtime == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (runtime->context != NULL) {
        for (size_t index = 0; index < runtime->accepted_count; index++) {
            if (mtcp_close(runtime->context, runtime->accepted_sockets[index]) < 0) {
                result = -1;
            }
        }
        free(runtime->accepted_sockets);
        runtime->accepted_sockets = NULL;
        runtime->accepted_count = 0;

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
        runtime->accepted_sockets != NULL || runtime->accepted_count != 0) {
        errno = EALREADY;
        return -1;
    }

    if (mtcp_init(config_file) != 0) {
        return -1;
    }
    runtime->global_initialized = 1;

    runtime->context = mtcp_create_context(0);
    if (runtime->context == NULL) {
        goto fail;
    }

    runtime->epoll_fd = mtcp_epoll_create(runtime->context, 1);
    if (runtime->epoll_fd < 0) {
        goto fail;
    }

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

    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, ipv4_address, &address.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }

    runtime->listener_socket = mtcp_socket(runtime->context, AF_INET,
                                            SOCK_STREAM, 0);
    if (runtime->listener_socket < 0 ||
        mtcp_bind(runtime->context, runtime->listener_socket,
                  (const struct sockaddr *)&address, sizeof(address)) < 0 ||
        mtcp_listen(runtime->context, runtime->listener_socket, backlog) < 0) {
        return -1;
    }

    return 0;
}

int vr_mtcp_runtime_accept_one(vr_mtcp_runtime_t *runtime,
                               int *accepted_socket)
{
    int socket_id;
    int *accepted_sockets;

    if (runtime == NULL || runtime->context == NULL || runtime->epoll_fd < 0 ||
        runtime->listener_socket < 0 || accepted_socket == NULL) {
        errno = EINVAL;
        return -1;
    }

    *accepted_socket = -1;
    socket_id = mtcp_accept(runtime->context, runtime->listener_socket,
                            NULL, NULL);
    if (socket_id < 0) {
        return -1;
    }

    accepted_sockets = realloc(runtime->accepted_sockets,
                                (runtime->accepted_count + 1) *
                                sizeof(*runtime->accepted_sockets));
    if (accepted_sockets == NULL) {
        int saved_errno = errno;
        mtcp_close(runtime->context, socket_id);
        errno = saved_errno;
        return -1;
    }

    runtime->accepted_sockets = accepted_sockets;
    runtime->accepted_sockets[runtime->accepted_count++] = socket_id;
    *accepted_socket = socket_id;
    return 1;
}