#ifndef SOCKET_UTILS_H
#define SOCKET_UTILS_H

#include "velora/common.h"
#include "velora/net.h"
#include <unistd.h>
#include <fcntl.h>

vr_result_t vr_server_addr_init(struct sockaddr_in *addr, uint16_t port);
int vr_socket_create(void);
vr_result_t vr_socket_set_reuseaddr(int fd);
vr_result_t vr_socket_bind(struct sockaddr_in *addr, int fd);
vr_result_t vr_socket_listen(int fd);
ssize_t vr_socket_recv(int fd, void *buf, size_t len, int flags);
ssize_t vr_socket_send(int fd, const void *buf, size_t len, int flags);
ssize_t vr_socket_send_all(int fd, const void *buf, size_t *len, int flags);
vr_result_t vr_socket_set_non_blocking(int sockfd);

#endif