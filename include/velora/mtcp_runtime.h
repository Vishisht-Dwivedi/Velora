#ifndef VR_MTCP_RUNTIME_H
#define VR_MTCP_RUNTIME_H

#include <stddef.h>

#include <mtcp_api.h>

typedef struct {
    mctx_t context;
    int epoll_fd;
    int global_initialized;
} vr_mtcp_runtime_t;

#define VR_MTCP_RUNTIME_INITIALIZER { NULL, -1, 0 }

int vr_mtcp_runtime_init(vr_mtcp_runtime_t *runtime, const char *config_file);
int vr_mtcp_runtime_destroy(vr_mtcp_runtime_t *runtime);

#endif