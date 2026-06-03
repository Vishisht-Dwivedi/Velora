#include "velora/common.h"
#include "velora/logger.h"
#include "velora/net.h"
#include <unistd.h>

int main()
{
    vr_log_init();
    int fd;
    int port = 22409;
    if ((vr_tcp_server_create(port, &fd)) == VR_SUCCESS)
    {
        vr_log(VR_LOG_INFO, "Server Started on port: %d", port);
    }
    pause();
    vr_log_close();
}