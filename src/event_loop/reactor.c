#include "velora/reactor.h"

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