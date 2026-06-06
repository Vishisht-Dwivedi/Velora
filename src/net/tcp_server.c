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