#include "velora/mtcp_runtime.h"

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
    int result = 1;

    if (argc != 4 || parse_port(argv[3], &port) < 0) {
        fprintf(stderr,
                "Usage: %s <mtcp-config-file> <bind-ipv4> <port>\n",
                argv[0]);
        return 2;
    }

    if (vr_mtcp_runtime_init(&runtime, argv[1]) < 0) {
        perror("mTCP runtime initialization failed");
        return 1;
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
    result = 0;

cleanup:
    if (vr_mtcp_runtime_destroy(&runtime) < 0) {
        perror("mTCP runtime cleanup failed");
        return 1;
    }

    return result;
}