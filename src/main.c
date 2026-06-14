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
    int listen_fd;
    int port = 22409;
    if ((vr_tcp_server_create(port, &listen_fd)) == VR_SUCCESS)
    {
        vr_log(VR_LOG_INFO, "Server Started on port: %d", port);
    }
    if((vr_socket_set_non_blocking(listen_fd)) == VR_SUCCESS)
    {
        vr_log(VR_LOG_INFO, "Set listening socket to non blocking");
    }
    vr_reactor_add(&reactor, listen_fd, EPOLLIN);
    vr_connection_t conn;
    char buffer[VR_IO_BUFFER_SIZE + 1];
    while (running_status == RUNNING)
    {
        vr_reactor_wait(&reactor, -1);
        for (int i = 0; i < reactor.ready_events; i++)
        {
            int ready_fd = reactor.events[i].data.fd;
            vr_log(VR_LOG_INFO, "Ready fd: %d", ready_fd);
            if (ready_fd == listen_fd)
            {
                if((vr_tcp_accept(ready_fd, &conn) == VR_ERROR))
                    break;
                vr_socket_set_non_blocking(conn.fd);
                vr_reactor_add(&reactor, conn.fd, EPOLLIN);
            }
            else
            {
                ssize_t len = vr_socket_recv(ready_fd, (void *)buffer, VR_IO_BUFFER_SIZE, 0);
                if (len == 0)
                {
                    vr_log(VR_LOG_INFO, "Client Disconnected");
                    vr_reactor_remove(&reactor, ready_fd);
                    close(ready_fd);
                    continue;
                }
                if (len == -1)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        continue;
                    }
                    else
                    {
                        vr_reactor_remove(&reactor, ready_fd);
                        close(ready_fd);
                        continue;
                    }
                }
                buffer[len] = '\0';
                vr_log(VR_LOG_INFO, "Received bytes: %s", buffer);
            }
        }
        reactor.ready_events = 0;
    }
    close(listen_fd);
    vr_reactor_destroy(&reactor);
    vr_log_close();
}