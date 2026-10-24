#include "velora/logger.h"
#include "velora/mtcp_runtime.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static volatile sig_atomic_t shutdown_requested;

static void request_shutdown(int signal_number)
{
    (void)signal_number;
    shutdown_requested = 1;
}

static int install_shutdown_handlers(void)
{
    struct sigaction action = { 0 };

    action.sa_handler = request_shutdown;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        return -1;
    }
    return 0;
}

static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    for (const char *digit = text; *digit != '\0'; digit++) {
        if (*digit < '0' || *digit > '9') {
            return -1;
        }
    }

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 ||
        value > UINT16_MAX) {
        return -1;
    }

    *port = (uint16_t)value;
    return 0;
}

int main(int argc, char **argv)
{
    vr_mtcp_runtime_t runtime = VR_MTCP_RUNTIME_INITIALIZER;
    uint16_t port;
    int runtime_initialized = 0;
    int exit_status = EXIT_FAILURE;

    if (argc != 4 || parse_port(argc == 4 ? argv[3] : NULL, &port) < 0) {
        fprintf(stderr, "Usage: %s <mtcp-config-file> <bind-ipv4> <port>\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    if (vr_log_init() != VR_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (install_shutdown_handlers() < 0) {
        perror("install signal handlers");
        goto cleanup;
    }
    if (vr_mtcp_runtime_init(&runtime, argv[1]) < 0) {
        perror("initialize mTCP runtime");
        goto cleanup;
    }
    runtime_initialized = 1;
    if (vr_mtcp_runtime_listen(&runtime, argv[2], port, 128) < 0) {
        perror("listen on mTCP runtime");
        goto cleanup;
    }

    while (!shutdown_requested) {
        for (;;) {
            int accepted_socket;
            int accept_result = vr_mtcp_runtime_accept_one(
                &runtime, &accepted_socket);
            if (accept_result < 0) {
                perror("accept mTCP connection");
                goto cleanup;
            }
            if (accept_result == 0) {
                break;
            }
        }

        size_t bytes_received;
        size_t packets_processed;
        size_t bytes_accepted_for_send;
        if (vr_mtcp_runtime_process_events(
                &runtime, 100, &bytes_received, &packets_processed,
                &bytes_accepted_for_send) < 0) {
            perror("process mTCP events");
            goto cleanup;
        }
    }

    exit_status = EXIT_SUCCESS;

cleanup:
    if (runtime_initialized && vr_mtcp_runtime_destroy(&runtime) < 0) {
        perror("destroy mTCP runtime");
        exit_status = EXIT_FAILURE;
    }
    vr_log_close();
    return exit_status;
}