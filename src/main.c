#include "velora/common.h"
#include "velora/logger.h"
#include "velora/net.h"
#include <unistd.h>
#include <signal.h>

enum State
{
    RUNNING,
    EXITED
};

volatile sig_atomic_t running_status = RUNNING;
void handle_sigint(int sig)
{
    printf("Interrupt signal %d\n", sig);
    running_status = EXITED;
}
int main()
{
    vr_log_init();
    int fd;
    int port = 22409;
    if ((vr_tcp_server_create(port, &fd)) == VR_SUCCESS)
    {
        vr_log(VR_LOG_INFO, "Server Started on port: %d", port);
    }
    vr_connection_t conn;
    signal(SIGINT, handle_sigint);
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