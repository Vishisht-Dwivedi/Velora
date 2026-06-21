#ifndef VR_CONN_H
#define VR_CONN_H
#include "velora/common.h"
#include "velora/net.h"
#include "velora/logger.h"
#include "velora/error.h"
#include "velora/parser.h"

#define VR_MAX_CONNECTIONS 65536
#define VR_CONNECTION_BUFFER_INITIAL_SIZE 4096
#define VR_CONNECTION_BUFFER_MAX_SIZE 65536

typedef enum
{
    VR_CONN_RING_BUF_UNALLOC,
    VR_CONN_RING_BUF_ACTIVE,
    VR_CONN_RING_BUF_FULL
} vr_connection_ring_buf_state_t;

typedef struct
{
    uint8_t *data;
    uint32_t capacity;
    uint32_t count;
    uint16_t read_pos;
    uint16_t write_pos;
    uint8_t state;
} vr_connection_ring_buf_t;

typedef enum
{
    VR_CONN_RING_BUF_READ,
    VR_CONN_RING_BUF_WRITE
}
vr_connection_ring_buf_type_t;

typedef enum
{
    VR_ACTIVE,
    VR_CLOSING,
    VR_CLOSED
} vr_connection_status_t;

typedef enum
{
    VR_CONN_LISTENER,
    VR_CONN_CLIENT
} vr_connection_type_t;

typedef struct vr_connection
{
    vr_net_conn_t net_conn;
    vr_connection_status_t status;
    vr_connection_type_t type;
    size_t slot;
    vr_connection_ring_buf_t read_buf;
    vr_connection_ring_buf_t write_buf;
    vr_parser_t parser;
} vr_connection_t;

//since epoll now handles pointers.. 
//moving and reallocating those arrays will change refs
//thus we store ptrs to arrays and handle ptrs only..
typedef struct
{
    vr_connection_t **slots;
    size_t count;
    size_t capacity;
} vr_connection_manager_t;

vr_result_t vr_connection_manager_init(vr_connection_manager_t *manager, size_t capacity);
vr_result_t vr_connection_manager_destroy(vr_connection_manager_t *manager);
vr_connection_t *vr_connection_create(vr_connection_manager_t *manager, vr_net_conn_t conn);
vr_result_t vr_connection_destroy(vr_connection_manager_t *manager, vr_connection_t *conn);

vr_result_t vr_conn_ring_buf_init(vr_connection_ring_buf_t *buf);
uint32_t vr_conn_ring_buf_size(vr_connection_ring_buf_t *buf);
uint32_t vr_conn_ring_buf_free(vr_connection_ring_buf_t *buf);
bool vr_conn_ring_buf_empty(vr_connection_ring_buf_t *buf);
bool vr_conn_ring_buf_full(vr_connection_ring_buf_t *buf);
vr_result_t vr_conn_ring_buf_push(vr_connection_ring_buf_t *buf, uint8_t data);
vr_result_t vr_conn_ring_buf_pop(vr_connection_ring_buf_t *buf, uint8_t *data);
vr_result_t vr_conn_ring_buf_grow(vr_connection_ring_buf_t *buf);

#endif
