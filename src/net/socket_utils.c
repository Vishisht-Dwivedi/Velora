#include "velora/socket_utils.h"
#include "velora/logger.h"
#include "velora/error.h"

int vr_server_addr_init(struct sockaddr_in *addr, uint16_t port)
{
    if(addr == NULL)
    {
        vr_log(VR_LOG_ERROR, "NULL address passed to vr_tcp_init");
        return VR_ERROR;
    }
    memset(addr, 0, sizeof(*addr));
    addr->sin_port = htons(port);
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(INADDR_ANY);
    vr_log(VR_LOG_INFO, "Setup sockaddr struct correctly");
    return VR_SUCCESS;
}

int vr_socket_create(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(fd == -1)
    {
        vr_perror(vr_error_string(VR_ERR_SOCKET_CREATE));
        return VR_ERROR;
    }
    vr_log(VR_LOG_INFO, "Socket fd: %d", fd);
    return fd;
}

int vr_socket_bind(struct sockaddr_in *addr, int fd)
{
    int res = bind(fd, (struct sockaddr *)addr, sizeof(*addr));
    if (res == -1)
    {
        vr_perror(vr_error_string(VR_ERR_SOCKET_BIND));
        return VR_ERROR;
    }
    vr_log(VR_LOG_INFO, "Socket bind successful");
    return VR_SUCCESS;
}

int vr_socket_listen(int fd)
{
    int res = listen(fd, SOMAXCONN);
    if (res == -1)
    {
        vr_perror(vr_error_string(VR_ERR_SOCKET_LISTEN));
        return VR_ERROR;
    }
    vr_log(VR_LOG_INFO, "Listening on socket %d", fd);
    return VR_SUCCESS;
}

int vr_socket_set_reuseaddr(int fd) 
{
    int opt = 1;
    int res = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if(res == -1)
    {
        vr_perror("SO_REUSEADDR failed");
        return VR_ERROR;
    }
    vr_log(VR_LOG_INFO, "SO_REUSEADDR success");
    return VR_SUCCESS;
}