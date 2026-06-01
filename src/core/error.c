#include "velora/error.h"
#include "velora/logger.h"

const char *vr_error_string(vr_error_t err)
{
    switch (err)
    {
        case VR_OK:
            return "No error";
        case VR_ERR_LOG_INIT:
            return "Failed to initialize logger";
        case VR_ERR_MUTEX_INIT:
            return "Failed to initialize mutex";
        case VR_ERR_SOCKET_CREATE:
            return "Failed to create socket";
        case VR_ERR_SOCKET_BIND:
            return "Failed to bind socket";
        case VR_ERR_SOCKET_LISTEN:
            return "Failed to listen on socket";
        default:
            return "Unknown error";
    }
}

void vr_perror(const char *msg)
{
    vr_log(VR_LOG_ERROR, "%s: %s", msg, strerror(errno));
}
