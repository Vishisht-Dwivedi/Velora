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
} vr_reactor_t;

vr_result_t vr_reactor_create(vr_reactor_t *reactor);
vr_result_t vr_reactor_destroy(vr_reactor_t *reactor);
