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
