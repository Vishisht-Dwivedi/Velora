#ifndef VR_NET_H
#define VR_NET_H

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

vr_result_t vr_tcp_server_create(uint16_t port, int *socket_fd);

typedef struct 
{
    int fd;
    struct sockaddr_in addr;
} vr_connection_t;

vr_result_t vr_tcp_accept(int server_fd, vr_connection_t *conn);

#endif