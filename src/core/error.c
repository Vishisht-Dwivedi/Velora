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
        case VR_ERR_SOCKET_CONNECT:
            return "Failed to connect to the socket fd";
        case VR_ERR_SOCKET_ACCEPT:
            return "Failed to accept queued connection";
        case VR_ERR_SOCKET_BIND:
            return "Failed to bind socket";
        case VR_ERR_SOCKET_LISTEN:
            return "Failed to listen on socket";
        case VR_ERR_INVALID_ADDR:
            return "Error due to invalid address provided";
        case VR_ERR_INVALID_ADDR_FAMILY:
            return "Error due to unsupported address family";
        default:
            return "Unknown error";
    }
}

void vr_perror(const char *msg)
{
    vr_log(VR_LOG_ERROR, "%s: %s", msg, strerror(errno));
}
