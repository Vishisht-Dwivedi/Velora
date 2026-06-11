#include "velora/common.h"
#include "velora/logger.h"
#include "velora/net.h"
#include "velora/socket_utils.h"
#include "velora/error.h"
#include <unistd.h>
#include "velora/reactor.h"

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
    vr_reactor_t reactor;
    reactor.epoll_fd = -1;
    vr_reactor_create(&reactor);
    int fd;
    int port = 22409;
    if ((vr_tcp_server_create(port, &fd)) == VR_SUCCESS)
    {
        vr_log(VR_LOG_INFO, "Server Started on port: %d", port);
    }
    vr_reactor_add(&reactor, fd, EPOLLIN);
    vr_connection_t conn;
    while (running_status == RUNNING)
    {
        vr_reactor_wait(&reactor, -1);
        for (int i = 0; i < reactor.ready_events; i++)
        {
            int ready_fd = reactor.events[i].data.fd;
            vr_log(VR_LOG_INFO, "Ready fd: %d", ready_fd);
        }
        reactor.ready_events = 0;
    }
    close(fd);
    vr_reactor_destroy(&reactor);
    vr_log_close();
}