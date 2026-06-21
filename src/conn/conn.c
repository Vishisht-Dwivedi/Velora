#include "velora/conn.h"
#include "velora/logger.h"

vr_result_t vr_connection_manager_init(vr_connection_manager_t *manager, size_t capacity)
{
    manager->count = 0;
    manager->capacity = capacity;
    manager->slots = calloc(capacity, sizeof(vr_connection_t *));
    if (manager->slots == NULL)
    {
        vr_perror("Connection manager allocation failed");
        return VR_ERROR;
    }
    return VR_SUCCESS;
}

vr_result_t vr_connection_manager_destroy(vr_connection_manager_t *manager)
{
    for (size_t i = 0; i < manager->count; i++)
    {
        free(manager->slots[i]->read_buf.data);
        free(manager->slots[i]->write_buf.data);
        free(manager->slots[i]);
    }
    free(manager->slots);
    manager->slots = NULL;
    manager->count = 0;
    manager->capacity = 0;
    return VR_SUCCESS;
}
vr_connection_t *vr_connection_create(vr_connection_manager_t *manager, vr_net_conn_t conn)
{
    if (manager == NULL || manager->slots == NULL)
    {
        vr_log(VR_LOG_ERROR, "Uninitialized connection manager");
        return NULL;
    }
    if (manager->count == manager->capacity)
    {
        size_t new_capacity = (manager->capacity == 0) ? 16 : manager->capacity * 2;
        if (vr_util_array_extend((void **)&manager->slots, new_capacity, sizeof(vr_connection_t *)) == VR_ERROR)
        {
            return NULL;
        }
        manager->capacity = new_capacity;
    }
    vr_connection_t *new_connection = malloc(sizeof(vr_connection_t));
    if (new_connection == NULL)
    {
        vr_perror("Memory allocation failed for new connection");
        return NULL;
    }
    manager->slots[manager->count++] = new_connection;
    memset(new_connection, 0, sizeof(vr_connection_t));
    new_connection->net_conn = conn;
    new_connection->slot = manager->count - 1;
    new_connection->status = VR_ACTIVE;
    new_connection->parser.state = VR_PARSER_HEADER_WAIT;
    return new_connection;
}

vr_result_t vr_connection_destroy(vr_connection_manager_t *manager, vr_connection_t *conn)
{
    if(manager == NULL || conn == NULL)
    {
        vr_log(VR_LOG_ERROR, "Invalid parameters");
        return VR_ERROR;
    }
    size_t slot = conn->slot;
    size_t last = manager->count - 1;
    if(slot != last)
    {
        manager->slots[slot] = manager->slots[last];
        manager->slots[slot]->slot = slot;
    }
    free(conn->read_buf.data);
    free(conn->write_buf.data);
    free(conn);
    manager->count--;
    vr_log(VR_LOG_INFO, "Removed connection from manager successfully");
    return VR_SUCCESS;
}