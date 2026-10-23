#include "velora/mtcp_runtime.h"
#include "velora/logger.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static int parse_port(const char *text, unsigned long *port)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || text[0] == '\0' || *end != '\0' || parsed == 0 ||
        parsed > 65535) {
        return -1;
    }

    *port = parsed;
    return 0;
}

int main(int argc, char **argv)
{
    vr_mtcp_runtime_t runtime = VR_MTCP_RUNTIME_INITIALIZER;
    unsigned long port;
    int accepted_socket;
    size_t bytes_received;
    size_t packets_processed;
    size_t bytes_accepted_for_send;
    size_t total_bytes_received = 0;
    size_t total_packets_processed = 0;
    size_t total_bytes_accepted_for_send = 0;
    int result = 1;
    int logger_initialized = 0;

    if (argc != 4 || parse_port(argv[3], &port) < 0) {
        fprintf(stderr,
                "Usage: %s <mtcp-config-file> <bind-ipv4> <port>\n",
                argv[0]);
        return 2;
    }

    if (vr_log_init() == VR_ERROR) {
        fprintf(stderr, "Velora logger initialization failed\n");
        return 1;
    }
    logger_initialized = 1;

    if (vr_mtcp_runtime_init(&runtime, argv[1]) < 0) {
        perror("mTCP runtime initialization failed");
        goto cleanup;
    }

    if (vr_mtcp_runtime_listen(&runtime, argv[2], (uint16_t)port, 16) < 0) {
        perror("mTCP listener setup failed");
        goto cleanup;
    }

    printf("Listening on %s:%lu; waiting for one client\n", argv[2], port);
    fflush(stdout);

    if (vr_mtcp_runtime_accept_one(&runtime, &accepted_socket) < 0) {
        perror("mTCP accept failed");
        goto cleanup;
    }

    printf("Accepted mTCP socket %d\n", accepted_socket);

    do {
        if (vr_mtcp_runtime_process_events(&runtime, -1, &bytes_received,
                                           &packets_processed,
                                           &bytes_accepted_for_send) < 0) {
            perror("mTCP request/response processing failed");
            goto cleanup;
        }
        total_bytes_received += bytes_received;
        total_packets_processed += packets_processed;
        total_bytes_accepted_for_send += bytes_accepted_for_send;
    } while (total_bytes_accepted_for_send == 0);

    printf("Received %zu bytes; parsed %zu packets; mTCP accepted %zu response bytes\n",
           total_bytes_received, total_packets_processed,
           total_bytes_accepted_for_send);
    result = 0;

cleanup:
    if (vr_mtcp_runtime_destroy(&runtime) < 0) {
        perror("mTCP runtime cleanup failed");
        result = 1;
    }
    if (logger_initialized) {
        vr_log_close();
    }

    return result;
}