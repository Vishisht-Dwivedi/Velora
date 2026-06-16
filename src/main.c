#include "velora/common.h"
#include "velora/logger.h"
#include "velora/net.h"
#include "velora/socket_utils.h"
#include "velora/error.h"
#include <unistd.h>
#include "velora/reactor.h"

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
    vr_net_conn_t conn;
    int listen_fd = 0;
    int port = 22409;
    vr_reactor_loop(&reactor, &conn, listen_fd, port);
    vr_log_close();
}