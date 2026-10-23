#include "velora/mtcp_runtime.h"

#include <errno.h>

#include <mtcp_epoll.h>

int vr_mtcp_runtime_destroy(vr_mtcp_runtime_t *runtime)
{
    int result = 0;

    if (runtime == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (runtime->context != NULL && runtime->epoll_fd >= 0 &&
        mtcp_close(runtime->context, runtime->epoll_fd) < 0) {
        result = -1;
    }
    runtime->epoll_fd = -1;

    if (runtime->context != NULL) {
        mtcp_destroy_context(runtime->context);
        runtime->context = NULL;
    }

    if (runtime->global_initialized) {
        mtcp_destroy();
        runtime->global_initialized = 0;
    }

    return result;
}

int vr_mtcp_runtime_init(vr_mtcp_runtime_t *runtime, const char *config_file)
{
    if (runtime == NULL || config_file == NULL || config_file[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (runtime->context != NULL || runtime->epoll_fd >= 0 ||
        runtime->global_initialized) {
        errno = EALREADY;
        return -1;
    }

    if (mtcp_init(config_file) != 0) {
        return -1;
    }
    runtime->global_initialized = 1;

    runtime->context = mtcp_create_context(0);
    if (runtime->context == NULL) {
        goto fail;
    }

    runtime->epoll_fd = mtcp_epoll_create(runtime->context, 1);
    if (runtime->epoll_fd < 0) {
        goto fail;
    }

    return 0;

fail:
    vr_mtcp_runtime_destroy(runtime);
    return -1;
}