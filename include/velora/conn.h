#ifndef VR_CONN_H
#define VR_CONN_H
#include "velora/common.h"
#include "velora/net.h"
#include "velora/logger.h"

typedef struct 
{
    uint8_t *data;
    size_t capacity;
    size_t used;
} vr_connection_buffer_t;

typedef enum
{
    VR_ACTIVE,
    VR_CLOSING,
    VR_CLOSED
} vr_connection_status_t;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} vr_connection_handle_t;

typedef struct
{
    vr_net_conn_t net_conn;
    vr_connection_status_t status;
    vr_connection_buffer_t read_buf;
    vr_connection_buffer_t write_buf;
} vr_connection_t;

typedef struct 
{
    vr_connection_t connection;
    uint32_t generation;
} vr_connection_slot_t;

typedef struct
{
    vr_connection_slot_t *slots;
    size_t count;
    size_t capacity;
} vr_connection_manager_t;

vr_result_t vr_connection_manager_init(vr_connection_manager_t *manager, size_t capacity);
vr_result_t vr_connection_manager_destroy(vr_connection_manager_t *manager);
vr_connection_t *vr_connection_create(vr_connection_manager_t *manager, vr_net_conn_t conn);

#endif
