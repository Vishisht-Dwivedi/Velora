#include "velora/common.h"
#include "velora/logger.h"
#include "velora/net.h"
#include <unistd.h>


volatile sig_atomic_t running_status = RUNNING;

void handle_shutdown(int signal)
{
    (void)signal;
    write(STDOUT_FILENO, "Shutdown requested\n", 20);
    running_status = SHUTDOWN_REQUESTED;
}

static void setup_signals(void)
{
    struct sigaction sa = { 0 };
    sa.sa_handler = &handle_shutdown;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

int main()
{
    setup_signals();
    vr_log_init();
    int fd;
    int port = 22409;
    if ((vr_tcp_server_create(port, &fd)) == VR_SUCCESS)
    {
        vr_log(VR_LOG_INFO, "Server Started on port: %d", port);
    }
    vr_connection_t conn;
    while (running_status == RUNNING)
    {
        if(vr_tcp_accept(fd, &conn) != VR_SUCCESS)
            continue;
        printf("Connection to client estabilished\n");
        close(conn.fd);
    }
    close(fd);
    vr_log_close();
}