#include "velora/common.h"
#include "velora/net.h"
#include "velora/error.h"
#include "velora/logger.h"
#include "velora/socket_utils.h"

vr_result_t vr_tcp_server_create(uint16_t port, int *socket_fd)
{
    if (socket_fd == NULL)
    {
        vr_log(VR_LOG_ERROR, "NULL socket_fd passed to vr_tcp_server_create");
        return VR_ERROR;
    }
    struct sockaddr_in addr;
    if(vr_server_addr_init(&addr, port) == VR_ERROR)
        return VR_ERROR;
    //get socket file descriptor
    if((*socket_fd = vr_socket_create()) == VR_ERROR)
        return VR_ERROR;
    
    //allow rapid rebinding after restart
    if((vr_socket_set_reuseaddr(*socket_fd)) == VR_ERROR)
    {
        close(*socket_fd);
        return VR_ERROR;
    }
    //bind to socket
    if((vr_socket_bind(&addr, *socket_fd)) == VR_ERROR)
    {
        close(*socket_fd);
        return VR_ERROR;
    }
    //listening on the socket
    if((vr_socket_listen(*socket_fd)) == VR_ERROR)
    {
        close(*socket_fd);
        return VR_ERROR;
    }
    return VR_SUCCESS;
}

vr_result_t vr_tcp_accept(int server_fd, vr_connection_t *conn)
{
    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    if ((client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len)) == -1)
    {
        vr_perror(vr_error_string(VR_ERR_SOCKET_ACCEPT));
        return VR_ERROR;
    }
    uint16_t port = ntohs(client_addr.sin_port);
    char ip_str[INET_ADDRSTRLEN];
    if (inet_ntop(client_addr.sin_family, &(client_addr.sin_addr), ip_str, sizeof(ip_str)) == NULL)
    {
        close(client_fd);
        vr_log(VR_LOG_ERROR, "Address conversion failed for address: %d", client_addr.sin_addr.s_addr);
        return VR_ERROR;
    }
    vr_log(
        VR_LOG_INFO,
        "Connection to client established for client ip: %s:%d at fd: %d",
        ip_str,
        port,
        client_fd
    );
    conn->fd = client_fd;
    conn->addr = client_addr;
    return VR_SUCCESS;
}