#include "velora/conn.h"
#include "velora/socket_utils.h"

vr_result_t vr_conn_ring_buf_init(vr_connection_ring_buf_t *buf)
{
    if(buf->data != NULL)
    {
        vr_log(VR_LOG_ERROR, "Buffer already allocated");
        return VR_ERROR;
    }
    buf->capacity = VR_CONNECTION_BUFFER_INITIAL_SIZE;
    buf->data = malloc(buf->capacity);
    if(buf->data == NULL)
    {
        vr_perror("Memory allocation for ring buf failed");
        return VR_ERROR;
    }
    buf->count = 0;
    buf->read_pos = 0;
    buf->write_pos = 0;
    buf->state = VR_CONN_RING_BUF_ACTIVE;
    return VR_SUCCESS;
}

uint32_t vr_conn_ring_buf_size(vr_connection_ring_buf_t *buf)
{
    return buf->count;
}

uint32_t vr_conn_ring_buf_free(vr_connection_ring_buf_t *buf)
{
    return buf->capacity - buf->count;
}

bool vr_conn_ring_buf_empty(vr_connection_ring_buf_t *buf)
{
    return (buf->count == 0);
}

bool vr_conn_ring_buf_full(vr_connection_ring_buf_t *buf)
{
    return (buf->count == buf->capacity);
}

vr_result_t vr_conn_ring_buf_push(vr_connection_ring_buf_t *buf, uint8_t data)
{   
    if (buf->capacity == 0)
    {
        if(vr_conn_ring_buf_init(buf) == VR_ERROR)
        {
            buf->state = VR_CONN_RING_BUF_UNALLOC;
            return VR_ERROR;
        }
    }
    if (buf->count == buf->capacity)
    {
        //let protocol handler deal with expansion
        buf->state = VR_CONN_RING_BUF_FULL;
        return VR_ERROR;
    }
    buf->count++;
    buf->data[buf->write_pos] = data;
    buf->write_pos = (buf->write_pos + 1) % buf->capacity;
    return VR_SUCCESS;
}

vr_result_t vr_conn_ring_buf_pop(vr_connection_ring_buf_t *buf, uint8_t *data)
{
    if(buf->count == 0)
        return VR_ERROR;
    buf->count--;
    *data = buf->data[buf->read_pos];
    buf->read_pos = (buf->read_pos + 1) % buf->capacity;
    return VR_SUCCESS;
}

vr_result_t vr_conn_ring_buf_peek(vr_connection_ring_buf_t *buf, uint8_t *data)
{
    if(buf->count == 0)
        return VR_ERROR;
    *data = buf->data[buf->read_pos];
    return VR_SUCCESS;
}

vr_result_t vr_conn_ring_buf_grow(vr_connection_ring_buf_t *buf)
{
    if (buf == NULL)
        return VR_ERROR;
    if (buf->capacity == 0)
        return vr_conn_ring_buf_init(buf);
    if (buf->capacity >= VR_CONNECTION_BUFFER_MAX_SIZE)
    {
        errno = ENOMEM;
        return VR_ERROR;
    }
    uint32_t new_capacity = buf->capacity * 2;
    if (new_capacity > VR_CONNECTION_BUFFER_MAX_SIZE)
        new_capacity = VR_CONNECTION_BUFFER_MAX_SIZE;

    uint8_t *new_buf = malloc(new_capacity);
    if (new_buf == NULL)
    {
        vr_perror("Failed to allocate expanded ring buffer");
        return VR_ERROR;
    }
    if (buf->count > 0)
    {
        if (buf->read_pos < buf->write_pos)
        {
            memcpy(new_buf, buf->data + buf->read_pos, buf->count);
        }
        else
        {
            uint32_t first_chunk = buf->capacity - buf->read_pos;
            memcpy(new_buf, buf->data + buf->read_pos, first_chunk);
            memcpy(new_buf + first_chunk, buf->data, buf->count - first_chunk);
        }
    }
    free(buf->data);
    buf->data = new_buf;
    buf->capacity = new_capacity;
    buf->read_pos = 0;
    buf->write_pos = buf->count;
    buf->state = VR_CONN_RING_BUF_ACTIVE;
    return VR_SUCCESS;
}