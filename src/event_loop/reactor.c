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

vr_result_t vr_reactor_add(vr_reactor_t *reactor, int client_fd, u_int32_t events)
{
    if (reactor == NULL)
    {
        vr_log(VR_LOG_ERROR, "NULL reactor passed");
        return VR_ERROR;
    }
    struct epoll_event ev = {0};
    ev.events = events;
    ev.data.fd = client_fd;
    int res = epoll_ctl(reactor->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
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

vr_result_t vr_reactor_remove(vr_reactor_t *reactor, int client_fd)
{
    if (reactor == NULL)
    {
        vr_log(VR_LOG_ERROR, "NULL reactor passed");
        return VR_ERROR;
    }
    int res = epoll_ctl(reactor->epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
    if(res == -1)
    {
        vr_perror("Error on deleting fd from reactor events");
        return VR_ERROR;
    }
    vr_log(VR_LOG_INFO, "Removed element from reactor loop successfully");
    return VR_SUCCESS;    
}

vr_result_t vr_reactor_loop(vr_reactor_t *reactor, vr_net_conn_t *conn, int listen_fd, int port)
{
    reactor->epoll_fd = -1;
    vr_reactor_create(reactor);
    if ((vr_tcp_server_create(port, &listen_fd)) == VR_SUCCESS)
    {
        vr_log(VR_LOG_INFO, "Server Started on port: %d", port);
    }
    if((vr_socket_set_non_blocking(listen_fd)) == VR_SUCCESS)
    {
        vr_log(VR_LOG_INFO, "Set listening socket to non blocking");
    }
    vr_reactor_add(reactor, listen_fd, EPOLLIN | EPOLLET);
    char buffer[VR_IO_BUFFER_SIZE + 1];
    while (running_status == RUNNING)
    {
        vr_reactor_wait(reactor, -1);
        for (int i = 0; i < reactor->ready_events; i++)
        {
            int ready_fd = reactor->events[i].data.fd;
            vr_log(VR_LOG_INFO, "Ready fd: %d", ready_fd);
            if (ready_fd == listen_fd)
            {
                while (true)
                {
                    vr_result_t res = vr_tcp_accept(ready_fd, conn);
                    if (res == VR_SUCCESS)
                    {
                        vr_socket_set_non_blocking(conn->fd);
                        vr_reactor_add(reactor, conn->fd, EPOLLIN | EPOLLET);
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
            }
            else
            {
                size_t total = 0;
                bool disconnected = false;
                while (true)
                {
                    ssize_t len = vr_socket_recv(ready_fd, (void *)buffer, VR_IO_BUFFER_SIZE, 0);
                    if (len == 0)
                    {
                        vr_log(VR_LOG_INFO, "Client Disconnected");
                        vr_reactor_remove(reactor, ready_fd);
                        close(ready_fd);
                        disconnected = true;
                        break;
                    }
                    else if (len == -1)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            break;
                        }
                        else
                        {
                            vr_reactor_remove(reactor, ready_fd);
                            close(ready_fd);
                            break;
                        }
                    }
                    total += len;
                }
                if(!disconnected)
                    vr_log(VR_LOG_INFO, "Received %zu bytes", total);
            }
        }
        reactor->ready_events = 0;
    }
    close(listen_fd);
    vr_reactor_destroy(reactor);
    return VR_SUCCESS;
}