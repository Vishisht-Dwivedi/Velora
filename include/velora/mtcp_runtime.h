#ifndef VR_MTCP_RUNTIME_H
#define VR_MTCP_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "velora/conn.h"
#include <mtcp_api.h>

typedef struct {
    mctx_t context;
    int epoll_fd;
    int listener_socket;
    int global_initialized;
    vr_connection_manager_t connections;
    int connections_initialized;
} vr_mtcp_runtime_t;

#define VR_MTCP_RUNTIME_INITIALIZER { NULL, -1, -1, 0, { NULL, 0, 0 }, 0 }

int vr_mtcp_runtime_init(vr_mtcp_runtime_t *runtime, const char *config_file);
int vr_mtcp_runtime_listen(vr_mtcp_runtime_t *runtime,
                           const char *ipv4_address,
                           uint16_t port,
                           int backlog);
int vr_mtcp_runtime_accept_one(vr_mtcp_runtime_t *runtime,
                               int *accepted_socket);
int vr_mtcp_runtime_process_events(vr_mtcp_runtime_t *runtime,
                                   int timeout_ms,
                                   size_t *bytes_received,
                                   size_t *packets_processed);
int vr_mtcp_runtime_destroy(vr_mtcp_runtime_t *runtime);

#endif