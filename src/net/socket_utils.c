#include "velora/socket_utils.h"
#include "velora/logger.h"
#include "velora/error.h"

vr_result_t vr_server_addr_init(struct sockaddr_in *addr, uint16_t port)
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
        return -1;
    }
    vr_log(VR_LOG_INFO, "Socket fd: %d", fd);
    return fd;
}

vr_result_t vr_socket_bind(struct sockaddr_in *addr, int fd)
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

vr_result_t vr_socket_listen(int fd)
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

vr_result_t vr_socket_set_reuseaddr(int fd) 
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

ssize_t vr_socket_recv(int fd, void *buf, size_t len, int flags) 
{
    ssize_t received_len = recv(fd, buf, len, flags);
    if(received_len == -1)
    {
        vr_perror("Socket data reception error");
    }
    return received_len;
}

//for async later with partial send acceptance
ssize_t vr_socket_send(int fd, const void *buf, size_t len, int flags)
{
    ssize_t sent = send(fd, buf, len, flags);
    if (sent == -1)
    {
        vr_perror("Socket data sending error");
    }
    return sent;
}

//for sync blocking when all data must arrive
ssize_t vr_socket_send_all(int fd, const void *buf, size_t *len, int flags) 
{
    //len parameter behaves as both
    //in: bytes req
    //out: bytes sent
    size_t total = 0;
    size_t bytes_left = *len;
    ssize_t n = 0;
    while (total<*len)
    {
        n = vr_socket_send(fd, (const char *)buf + total, bytes_left, flags);
        if (n == -1)
        {
            return -1;
        }
        if (n == 0)
        {
            vr_log(VR_LOG_WARN, "Send() returned 0 bytes");
            return total;
        }
        total += n;
        bytes_left -= n;
    }
    *len = total;
    return total;
}
