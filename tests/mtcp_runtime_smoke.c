#include "velora/mtcp_runtime.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    vr_mtcp_runtime_t runtime = VR_MTCP_RUNTIME_INITIALIZER;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <mtcp-config-file>\n", argv[0]);
        return 2;
    }

    if (vr_mtcp_runtime_init(&runtime, argv[1]) < 0) {
        perror("mTCP runtime initialization failed");
        return 1;
    }

    if (vr_mtcp_runtime_destroy(&runtime) < 0) {
        perror("mTCP runtime cleanup failed");
        return 1;
    }

    return 0;
}