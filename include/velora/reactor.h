#include "common.h"
#include "error.h"
#include "logger.h"
#include "net.h"
#include "socket_utils.h"
#include <sys/epoll.h>

#define VR_REACTOR_MAX_EVENTS 1024

typedef struct
{
    int epoll_fd;
    struct epoll_event events[VR_REACTOR_MAX_EVENTS];
    int ready_events;
} vr_reactor_t;

vr_result_t vr_reactor_create(vr_reactor_t *reactor);
vr_result_t vr_reactor_destroy(vr_reactor_t *reactor);
vr_result_t vr_reactor_add(vr_reactor_t *reactor, int client_fd, u_int32_t events);
vr_result_t vr_reactor_wait(vr_reactor_t *reactor, int timeout);
