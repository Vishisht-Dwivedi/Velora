#include "velora/conn.h"

#include <assert.h>

static vr_connection_ring_buf_t make_ring(uint32_t capacity)
{
    vr_connection_ring_buf_t buffer = { 0 };

    buffer.data = malloc(capacity);
    assert(buffer.data != NULL);
    buffer.capacity = capacity;
    buffer.state = VR_CONN_RING_BUF_ACTIVE;
    return buffer;
}

static void push_all(vr_connection_ring_buf_t *buffer,
                     const uint8_t *bytes,
                     size_t length)
{
    for (size_t index = 0; index < length; index++)
        assert(vr_conn_ring_buf_push(buffer, bytes[index]) == VR_SUCCESS);
}

static void test_empty_full_partial_and_wraparound(void)
{
    const uint8_t initial[] = { 1, 2, 3, 4 };
    const uint8_t expected[] = { 3, 4, 5, 6 };
    vr_connection_ring_buf_t buffer = make_ring(4);
    uint8_t value;
    uint8_t *region = NULL;
    uint8_t copied[sizeof(expected)] = { 0 };

    assert(vr_conn_ring_buf_empty(&buffer));
    assert(!vr_conn_ring_buf_full(&buffer));
    assert(vr_conn_ring_buf_pop(&buffer, &value) == VR_ERROR);

    push_all(&buffer, initial, sizeof(initial));
    assert(vr_conn_ring_buf_full(&buffer));
    assert(vr_conn_ring_buf_free(&buffer) == 0);
    assert(vr_conn_ring_buf_push(&buffer, 5) == VR_ERROR);
    assert(vr_conn_ring_buf_commit(&buffer, 1) == VR_ERROR);

    assert(vr_conn_ring_buf_pop(&buffer, &value) == VR_SUCCESS && value == 1);
    assert(vr_conn_ring_buf_pop(&buffer, &value) == VR_SUCCESS && value == 2);
    assert(vr_conn_ring_buf_contiguous_write(&buffer, &region) == 2);
    assert(region != NULL);
    region[0] = 5;
    region[1] = 6;
    assert(vr_conn_ring_buf_commit(&buffer, 2) == VR_SUCCESS);
    assert(vr_conn_ring_buf_full(&buffer));

    assert(vr_conn_ring_buf_peek_n(&buffer, copied, sizeof(copied)) == VR_SUCCESS);
    assert(memcmp(copied, expected, sizeof(expected)) == 0);
    assert(vr_conn_ring_buf_contiguous_read(&buffer, &region) == 2);
    assert(region[0] == 3 && region[1] == 4);
    assert(vr_conn_ring_buf_consume(&buffer, 1) == VR_SUCCESS);
    assert(vr_conn_ring_buf_contiguous_read(&buffer, &region) == 1);
    assert(region[0] == 4);
    assert(vr_conn_ring_buf_consume(&buffer, 3) == VR_SUCCESS);
    assert(vr_conn_ring_buf_empty(&buffer));
    assert(vr_conn_ring_buf_consume(&buffer, 1) == VR_ERROR);

    free(buffer.data);
}

static void test_grow_preserves_wrapped_order(void)
{
    const uint8_t initial[] = { 10, 20, 30, 40 };
    const uint8_t expected[] = { 40, 50, 60, 70 };
    vr_connection_ring_buf_t buffer = make_ring(4);
    uint8_t value;
    uint8_t copied[sizeof(expected)] = { 0 };

    push_all(&buffer, initial, sizeof(initial));
    for (size_t index = 0; index < 3; index++)
        assert(vr_conn_ring_buf_pop(&buffer, &value) == VR_SUCCESS);
    push_all(&buffer, (const uint8_t[]){ 50, 60, 70 }, 3);
    assert(vr_conn_ring_buf_full(&buffer));

    assert(vr_conn_ring_buf_grow(&buffer) == VR_SUCCESS);
    assert(buffer.capacity == 8);
    assert(buffer.read_pos == 0);
    assert(buffer.write_pos == 4);
    assert(vr_conn_ring_buf_peek_n(&buffer, copied, sizeof(copied)) == VR_SUCCESS);
    assert(memcmp(copied, expected, sizeof(expected)) == 0);

    free(buffer.data);
}

static void test_reserve_capacity_boundary(void)
{
    vr_connection_ring_buf_t buffer = { 0 };

    assert(vr_conn_ring_buf_init(&buffer) == VR_SUCCESS);
    assert(vr_conn_ring_buf_reserve(
               &buffer,
               VR_CONNECTION_BUFFER_MAX_SIZE - VR_CONNECTION_BUFFER_INITIAL_SIZE) ==
           VR_SUCCESS);
    assert(buffer.capacity == VR_CONNECTION_BUFFER_MAX_SIZE);
    assert(vr_conn_ring_buf_free(&buffer) == VR_CONNECTION_BUFFER_MAX_SIZE);

    errno = 0;
    assert(vr_conn_ring_buf_reserve(&buffer,
                                    VR_CONNECTION_BUFFER_MAX_SIZE + 1) == VR_ERROR);
    assert(errno == ENOMEM);
    assert(buffer.capacity == VR_CONNECTION_BUFFER_MAX_SIZE);
    free(buffer.data);
}

static void initialize_connection_buffers(vr_connection_t *connection,
                                          uint8_t seed)
{
    assert(vr_conn_ring_buf_init(&connection->read_buf) == VR_SUCCESS);
    assert(vr_conn_ring_buf_init(&connection->write_buf) == VR_SUCCESS);
    assert(vr_conn_ring_buf_push(&connection->read_buf, seed) == VR_SUCCESS);
    assert(vr_conn_ring_buf_push(&connection->write_buf, (uint8_t)(seed + 1)) ==
           VR_SUCCESS);
}

static void test_repeated_connection_lifecycle(void)
{
    vr_connection_manager_t manager = { 0 };

    assert(vr_connection_manager_init(&manager, 1) == VR_SUCCESS);
    for (int cycle = 0; cycle < 128; cycle++)
    {
        vr_net_conn_t net_connection = { .fd = cycle };
        vr_connection_t *connection = vr_connection_create(&manager,
                                                           net_connection);
        assert(connection != NULL);
        initialize_connection_buffers(connection, (uint8_t)cycle);
        assert(vr_connection_destroy(&manager, connection) == VR_SUCCESS);
        assert(manager.count == 0);
    }

    vr_connection_t *partial_connection = vr_connection_create(
        &manager, (vr_net_conn_t){ .fd = 999 });
    assert(partial_connection != NULL);
    assert(vr_conn_ring_buf_init(&partial_connection->read_buf) == VR_SUCCESS);
    assert(partial_connection->write_buf.data == NULL);
    assert(vr_connection_manager_destroy(&manager) == VR_SUCCESS);
    assert(manager.slots == NULL);
}

static void test_connection_slot_compaction_and_manager_cleanup(void)
{
    vr_connection_manager_t manager = { 0 };
    vr_connection_t *connections[3];

    assert(vr_connection_manager_init(&manager, 1) == VR_SUCCESS);
    for (int index = 0; index < 3; index++)
    {
        vr_net_conn_t net_connection = { .fd = index + 100 };
        connections[index] = vr_connection_create(&manager, net_connection);
        assert(connections[index] != NULL);
        initialize_connection_buffers(connections[index], (uint8_t)index);
    }

    assert(vr_connection_destroy(&manager, connections[1]) == VR_SUCCESS);
    assert(manager.count == 2);
    assert(manager.slots[1] == connections[2]);
    assert(manager.slots[1]->slot == 1);

    assert(vr_connection_manager_destroy(&manager) == VR_SUCCESS);
    assert(manager.slots == NULL);
    assert(manager.count == 0);
}

int main(void)
{
    test_empty_full_partial_and_wraparound();
    test_grow_preserves_wrapped_order();
    test_reserve_capacity_boundary();
    test_repeated_connection_lifecycle();
    test_connection_slot_compaction_and_manager_cleanup();
    return 0;
}
