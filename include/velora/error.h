#ifndef VR_ERROR_H
#define VR_ERROR_H

#include "velora/common.h"
#include <errno.h>
typedef enum
{
    VR_OK = 0,
    VR_ERR_LOG_INIT,
    VR_ERR_MUTEX_INIT,

    VR_ERR_SOCKET_CREATE,
    VR_ERR_SOCKET_BIND,
    VR_ERR_SOCKET_LISTEN
} vr_error_t;

const char *vr_error_string(vr_error_t err);

void vr_perror(const char *msg);

#endif
