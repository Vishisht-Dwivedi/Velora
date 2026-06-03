#ifndef VR_NET_H
#define VR_NET_H

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

int vr_tcp_server_create(uint16_t port, int *socket_fd);

#endif