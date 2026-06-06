#ifndef SOCKET_UTILS_H
#define SOCKET_UTILS_H

#include "velora/common.h"
#include "velora/net.h"
#include <unistd.h>

vr_result_t vr_server_addr_init(struct sockaddr_in *addr, uint16_t port);
vr_result_t vr_socket_create(void);
vr_result_t vr_socket_set_reuseaddr(int fd);
vr_result_t vr_socket_bind(struct sockaddr_in *addr, int fd);
vr_result_t vr_socket_listen(int fd);

#endif