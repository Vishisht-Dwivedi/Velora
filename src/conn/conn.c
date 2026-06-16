#include "velora/conn.h"

vr_result_t vr_connection_manager_init(vr_connection_manager_t *manager, size_t capacity)
{
    manager->count = 0;
    manager->capacity = capacity;
    manager->slots = calloc(capacity, sizeof(vr_connection_slot_t));
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
        free(manager->slots[i].connection.read_buf.data);
        free(manager->slots[i].connection.write_buf.data);
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
        if (vr_util_array_extend((void **)&manager->slots, new_capacity, sizeof(vr_connection_slot_t)) == VR_ERROR)
        {
            return NULL;
        }
        manager->capacity = new_capacity;
    }
    vr_connection_slot_t *slot = &manager->slots[manager->count++];
    memset(slot, 0, sizeof(*slot));
    slot->generation = 1;
    slot->connection.net_conn = conn;
    slot->connection.status = VR_ACTIVE;
    return &slot->connection;
}
