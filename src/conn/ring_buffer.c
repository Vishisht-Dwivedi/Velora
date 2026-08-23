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
        buf->capacity = 0;
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

uint32_t vr_conn_ring_buf_contiguous_read(vr_connection_ring_buf_t *buf, uint8_t **data)
{
    if (buf == NULL || data == NULL || buf->count == 0)
        return 0;
    *data = &buf->data[buf->read_pos];
    if (buf->read_pos < buf->write_pos)
        return buf->count;
    return buf->capacity - buf->read_pos;
}

vr_result_t vr_conn_ring_buf_consume(vr_connection_ring_buf_t *buf, uint32_t count)
{
    if (buf == NULL || count > buf->count)
        return VR_ERROR;
    buf->read_pos = (buf->read_pos + count) % buf->capacity;
    buf->count -= count;
    if (buf->count == 0)
    {
        buf->read_pos = 0;
        buf->write_pos = 0;
    }
    if (buf->state == VR_CONN_RING_BUF_FULL)
        buf->state = VR_CONN_RING_BUF_ACTIVE;
    return VR_SUCCESS;
}

/* Copy `len` readable bytes into a flat destination without consuming them.
 * Crosses the wrap boundary with at most two memcpy() calls. */
vr_result_t vr_conn_ring_buf_peek_n(vr_connection_ring_buf_t *buf, uint8_t *out, uint32_t len)
{
    if (buf == NULL || out == NULL)
        return VR_ERROR;
    if (len > buf->count)
        return VR_ERROR;
    if (len == 0)
        return VR_SUCCESS;

    uint32_t first_chunk = buf->capacity - buf->read_pos;
    if (first_chunk >= len)
    {
        memcpy(out, buf->data + buf->read_pos, len);
    }
    else
    {
        memcpy(out, buf->data + buf->read_pos, first_chunk);
        memcpy(out + first_chunk, buf->data, len - first_chunk);
    }
    return VR_SUCCESS;
}

/* Return the number of contiguous writable bytes starting at write_pos, and
 * a pointer to that region. Mirrors the region1 computation already used in
 * vr_socket_recv_ring_buf() for the read side. Returns 0 (leaving *data
 * untouched) when uninitialized or full. */
uint32_t vr_conn_ring_buf_contiguous_write(vr_connection_ring_buf_t *buf, uint8_t **data)
{
    if (buf == NULL || data == NULL || buf->capacity == 0)
        return 0;

    uint32_t free_space = buf->capacity - buf->count;
    if (free_space == 0)
        return 0;

    *data = &buf->data[buf->write_pos];

    if (buf->write_pos >= buf->read_pos)
    {
        uint32_t region1_len = buf->capacity - buf->write_pos;
        return (region1_len > free_space) ? free_space : region1_len;
    }

    uint32_t contiguous = buf->read_pos - buf->write_pos;
    return (contiguous > free_space) ? free_space : contiguous;
}

/* Advance write_pos/count after the caller has written `count` bytes
 * directly into the region handed back by contiguous_write(). */
vr_result_t vr_conn_ring_buf_commit(vr_connection_ring_buf_t *buf, uint32_t count)
{
    if (buf == NULL)
        return VR_ERROR;
    if (count == 0)
        return VR_SUCCESS;
    if (buf->capacity == 0 || count > buf->capacity - buf->count)
        return VR_ERROR;

    buf->write_pos = (buf->write_pos + count) % buf->capacity;
    buf->count += count;
    if (buf->count == buf->capacity)
        buf->state = VR_CONN_RING_BUF_FULL;
    return VR_SUCCESS;
}

/* Initialize/grow the write buffer until at least `needed` bytes are free.
 * Invariant: completes (or fails) before any bytes are written/committed,
 * so a failure leaves the buffer exactly as it was found. */
vr_result_t vr_conn_ring_buf_reserve(vr_connection_ring_buf_t *buf, uint32_t needed)
{
    if (buf == NULL)
        return VR_ERROR;

    if (buf->capacity == 0)
    {
        if (vr_conn_ring_buf_init(buf) == VR_ERROR)
            return VR_ERROR;
    }

    while (vr_conn_ring_buf_free(buf) < needed)
    {
        if (vr_conn_ring_buf_grow(buf) == VR_ERROR)
            return VR_ERROR;
    }

    return VR_SUCCESS;
}