#include "velora/conn.h"
#include <assert.h>

static void push_bytes(vr_connection_t *conn, const uint8_t *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++)
        assert(vr_conn_ring_buf_push(&conn->read_buf, bytes[i]) == VR_SUCCESS);
}

static void append_bytes(vr_connection_t *conn, const uint8_t *bytes, size_t len)
{
    size_t offset = 0;

    assert(len <= UINT32_MAX);
    assert(vr_conn_ring_buf_reserve(&conn->read_buf, (uint32_t)len) == VR_SUCCESS);
    while (offset < len)
    {
        uint8_t *region = NULL;
        uint32_t available = vr_conn_ring_buf_contiguous_write(
            &conn->read_buf, &region);
        size_t chunk = available < len - offset ? available : len - offset;

        assert(region != NULL && chunk > 0);
        memcpy(region, bytes + offset, chunk);
        assert(vr_conn_ring_buf_commit(&conn->read_buf,
                                       (uint32_t)chunk) == VR_SUCCESS);
        offset += chunk;
    }
}

static void test_serialization_and_types(void)
{
    uint8_t wire[VR_HDR_SIZE + 0x0102] = {0};
    uint8_t payload[0x0102] = {0};
    vr_connection_t conn = {0};
    vr_packet_t parsed = {0};
    vr_parser_t parser = {.state = VR_PARSER_HEADER_WAIT};

    assert(vr_conn_ring_buf_init(&conn.read_buf) == VR_SUCCESS);

    for (uint8_t type = VR_PKT_CONNECT; type <= VR_PKT_ERROR; type++)
    {
        vr_packet_t packet = {0};
        packet.header.magic = VR_MAGIC;
        packet.header.version = VR_PROTOCOL_VERSION;
        packet.header.type = (vr_packet_type_t)type;
        packet.header.stream_id = 0xA5;
        packet.header.flags = VR_FLAG_ACK_REQ;
        packet.header.payload_len = sizeof(payload);
        packet.payload = payload;

        vr_packet_serialize(&packet, wire);
        assert(wire[0] == 0x57 && wire[1] == 0x89);
        assert(wire[2] == VR_PROTOCOL_VERSION && wire[3] == type);
        assert(wire[4] == 0xA5 && wire[5] == VR_FLAG_ACK_REQ);
        assert(wire[6] == 1 && wire[7] == 2);
        assert(memcmp(wire + VR_HDR_SIZE, payload, sizeof(payload)) == 0);

        push_bytes(&conn, wire, sizeof(wire));
        assert(vr_parser_poll(&parser, &conn, &parsed) == VR_SUCCESS);
        assert(parsed.header.magic == VR_MAGIC);
        assert(parsed.header.version == VR_PROTOCOL_VERSION);
        assert(parsed.header.type == type);
        assert(parsed.header.stream_id == 0xA5);
        assert(parsed.header.flags == VR_FLAG_ACK_REQ);
        assert(parsed.header.payload_len == sizeof(payload));
        assert(memcmp(parsed.payload, payload, sizeof(payload)) == 0);
        free(parsed.payload);
        parsed.payload = NULL;
    }

    free(conn.read_buf.data);
}

static void test_one_byte_header_reads(void)
{
    const uint8_t wire[VR_HDR_SIZE] = {
        0x57, 0x89, 1, VR_PKT_PING, 7, 0, 0, 0
    };
    vr_connection_t conn = {0};
    vr_packet_t parsed = {0};
    vr_parser_t parser = {.state = VR_PARSER_HEADER_WAIT};

    assert(vr_conn_ring_buf_init(&conn.read_buf) == VR_SUCCESS);
    for (size_t i = 0; i < sizeof(wire); i++)
    {
        push_bytes(&conn, &wire[i], 1);
        vr_result_t result = vr_parser_poll(&parser, &conn, &parsed);
        assert(result == (i + 1 == sizeof(wire) ? VR_SUCCESS : VR_EMPTY));
    }
    assert(parsed.header.type == VR_PKT_PING);
    assert(parsed.header.stream_id == 7);
    assert(parsed.payload == NULL);
    free(conn.read_buf.data);
}

static void test_empty_input_and_fragmented_payload(void)
{
    const uint8_t wire[VR_HDR_SIZE + 4] = {
        0x57, 0x89, 1, VR_PKT_PUBLISH, 3, 0, 0, 4,
        0x10, 0x20, 0x30, 0x40
    };
    vr_connection_t conn = {0};
    vr_packet_t parsed = {0};
    vr_parser_t parser = {.state = VR_PARSER_HEADER_WAIT};

    assert(vr_conn_ring_buf_init(&conn.read_buf) == VR_SUCCESS);
    assert(vr_parser_poll(&parser, &conn, &parsed) == VR_EMPTY);
    assert(vr_conn_ring_buf_empty(&conn.read_buf));

    push_bytes(&conn, wire, 2);
    assert(vr_parser_poll(&parser, &conn, &parsed) == VR_EMPTY);
    push_bytes(&conn, wire + 2, VR_HDR_SIZE - 2);
    assert(vr_parser_poll(&parser, &conn, &parsed) == VR_EMPTY);
    assert(parser.state == VR_PARSER_PAYLOAD_WAIT);
    assert(vr_conn_ring_buf_empty(&conn.read_buf));

    push_bytes(&conn, wire + VR_HDR_SIZE, 1);
    assert(vr_parser_poll(&parser, &conn, &parsed) == VR_EMPTY);
    push_bytes(&conn, wire + VR_HDR_SIZE + 1, 2);
    assert(vr_parser_poll(&parser, &conn, &parsed) == VR_EMPTY);
    push_bytes(&conn, wire + VR_HDR_SIZE + 3, 1);
    assert(vr_parser_poll(&parser, &conn, &parsed) == VR_SUCCESS);
    assert(parsed.header.type == VR_PKT_PUBLISH);
    assert(parsed.header.stream_id == 3);
    assert(parsed.header.payload_len == 4);
    assert(memcmp(parsed.payload, wire + VR_HDR_SIZE, 4) == 0);

    free(parsed.payload);
    free(conn.read_buf.data);
}

static void test_maximum_payload_length(void)
{
    const uint16_t payload_len = UINT16_MAX;
    uint8_t header[VR_HDR_SIZE] = {
        0x57, 0x89, 1, VR_PKT_PUBLISH, 4, 0, 0xff, 0xff
    };
    uint8_t *payload = malloc(payload_len);
    vr_connection_t conn = {0};
    vr_packet_t parsed = {0};
    vr_parser_t parser = {.state = VR_PARSER_HEADER_WAIT};

    assert(payload != NULL);
    for (size_t index = 0; index < payload_len; index++)
        payload[index] = (uint8_t)(index * 31u);

    assert(vr_conn_ring_buf_init(&conn.read_buf) == VR_SUCCESS);
    push_bytes(&conn, header, sizeof(header));
    assert(vr_parser_poll(&parser, &conn, &parsed) == VR_EMPTY);
    assert(parser.state == VR_PARSER_PAYLOAD_WAIT);
    append_bytes(&conn, payload, payload_len);
    assert(vr_parser_poll(&parser, &conn, &parsed) == VR_SUCCESS);
    assert(parsed.header.payload_len == UINT16_MAX);
    assert(memcmp(parsed.payload, payload, payload_len) == 0);

    free(parsed.payload);
    free(payload);
    free(conn.read_buf.data);
}

static void test_multiple_packets_and_partial_header(void)
{
    const uint8_t packets[VR_HDR_SIZE * 2] = {
        0x57, 0x89, 1, VR_PKT_PING, 1, 0, 0, 0,
        0x57, 0x89, 1, VR_PKT_PONG, 2, 0, 0, 0
    };
    vr_connection_t conn = {0};
    vr_packet_t parsed = {0};
    vr_parser_t parser = {.state = VR_PARSER_HEADER_WAIT};

    assert(vr_conn_ring_buf_init(&conn.read_buf) == VR_SUCCESS);
    push_bytes(&conn, packets, sizeof(packets));
    assert(vr_parser_poll(&parser, &conn, &parsed) == VR_SUCCESS);
    assert(parsed.header.type == VR_PKT_PING);
    assert(vr_parser_poll(&parser, &conn, &parsed) == VR_SUCCESS);
    assert(parsed.header.type == VR_PKT_PONG);

    push_bytes(&conn, packets, VR_HDR_SIZE - 1);
    assert(vr_parser_poll(&parser, &conn, &parsed) == VR_EMPTY);
    assert(vr_conn_ring_buf_size(&conn.read_buf) == VR_HDR_SIZE - 1);
    free(conn.read_buf.data);
}

static void test_invalid_header(void)
{
    const uint8_t valid[VR_HDR_SIZE] = {
        0x57, 0x89, 1, VR_PKT_PING, 0, 0, 0, 0
    };
    const size_t invalid_offsets[] = { 0, 2, 3, 5 };
    const uint8_t invalid_values[] = { 0, 2, 0, 0x80 };

    for (size_t index = 0;
         index < sizeof(invalid_offsets) / sizeof(invalid_offsets[0]);
         index++)
    {
        uint8_t invalid[VR_HDR_SIZE];
        vr_connection_t conn = {0};
        vr_packet_t parsed = {0};
        vr_parser_t parser = {.state = VR_PARSER_HEADER_WAIT};

        memcpy(invalid, valid, sizeof(invalid));
        invalid[invalid_offsets[index]] = invalid_values[index];
        assert(vr_conn_ring_buf_init(&conn.read_buf) == VR_SUCCESS);
        push_bytes(&conn, invalid, sizeof(invalid));
        assert(vr_parser_poll(&parser, &conn, &parsed) == VR_ERROR);
        assert(vr_conn_ring_buf_size(&conn.read_buf) == 0);
        free(parsed.payload);
        free(conn.read_buf.data);
    }
}

static void test_wrapped_header_and_partial_payload(void)
{
    const uint8_t wire[VR_HDR_SIZE + 3] = {
        0x57, 0x89, 1, VR_PKT_PUBLISH, 9, 0, 0, 3, 0x11, 0x22, 0x33
    };
    vr_connection_t conn = {0};
    vr_packet_t parsed = {0};
    vr_parser_t parser = {.state = VR_PARSER_HEADER_WAIT};

    conn.read_buf.capacity = 12;
    conn.read_buf.data = calloc(conn.read_buf.capacity, 1);
    assert(conn.read_buf.data != NULL);
    conn.read_buf.read_pos = 8;
    conn.read_buf.write_pos = 8;

    push_bytes(&conn, wire, VR_HDR_SIZE + 2);
    assert(vr_parser_poll(&parser, &conn, &parsed) == VR_EMPTY);
    assert(parser.state == VR_PARSER_PAYLOAD_WAIT);
    assert(vr_conn_ring_buf_size(&conn.read_buf) == 2);

    push_bytes(&conn, wire + VR_HDR_SIZE + 2, 1);
    assert(vr_parser_poll(&parser, &conn, &parsed) == VR_SUCCESS);
    assert(parsed.header.type == VR_PKT_PUBLISH);
    assert(parsed.header.payload_len == 3);
    assert(parsed.payload[0] == 0x11);
    assert(parsed.payload[1] == 0x22);
    assert(parsed.payload[2] == 0x33);
    free(parsed.payload);
    free(conn.read_buf.data);
}

static void test_stream_close_has_no_undefined_response(void)
{
    vr_connection_t conn = {0};
    vr_packet_t packet = {0};
    conn.proto_state = VR_PROTO_ESTABILISHED;
    conn.active_streams = 2;
    conn.streams_bitmap[0] = 3;
    packet.header.type = VR_PKT_STREAM_CLOSE;
    packet.header.stream_id = 1;

    assert(vr_protocol_handle_packet(&conn, &packet) == NULL);
    assert(conn.active_streams == 1);
    assert(conn.streams_bitmap[0] == 1);
    assert(conn.proto_state == VR_PROTO_ESTABILISHED);
}

static void test_duplicate_stream_close_is_ignored(void)
{
    vr_connection_t conn = {0};
    vr_packet_t packet = {0};
    conn.proto_state = VR_PROTO_ESTABILISHED;
    conn.active_streams = 2;
    conn.streams_bitmap[0] = 3;
    packet.header.type = VR_PKT_STREAM_CLOSE;
    packet.header.stream_id = 1;

    assert(vr_protocol_handle_packet(&conn, &packet) == NULL);
    assert(conn.active_streams == 1);
    assert(conn.streams_bitmap[0] == 1);
    assert(vr_protocol_handle_packet(&conn, &packet) == NULL);
    assert(conn.active_streams == 1);
    assert(conn.streams_bitmap[0] == 1);
}

static void test_unopened_stream_close_is_ignored(void)
{
    vr_connection_t conn = {0};
    vr_packet_t packet = {0};
    conn.proto_state = VR_PROTO_ESTABILISHED;
    conn.active_streams = 1;
    conn.streams_bitmap[0] = 1;
    packet.header.type = VR_PKT_STREAM_CLOSE;
    packet.header.stream_id = 2;

    assert(vr_protocol_handle_packet(&conn, &packet) == NULL);
    assert(conn.active_streams == 1);
    assert(conn.streams_bitmap[0] == 1);
}

static void test_default_stream_close_is_ignored(void)
{
    vr_connection_t conn = {0};
    vr_packet_t packet = {0};
    conn.proto_state = VR_PROTO_ESTABILISHED;
    conn.active_streams = 1;
    conn.streams_bitmap[0] = 1;
    packet.header.type = VR_PKT_STREAM_CLOSE;
    packet.header.stream_id = 0;

    assert(vr_protocol_handle_packet(&conn, &packet) == NULL);
    assert(conn.active_streams == 1);
    assert(conn.streams_bitmap[0] == 1);
}

static void test_stream_close_with_one_active_stream_is_ignored(void)
{
    vr_connection_t conn = {0};
    vr_packet_t packet = {0};
    conn.proto_state = VR_PROTO_ESTABILISHED;
    conn.active_streams = 1;
    conn.streams_bitmap[0] = 3;
    packet.header.type = VR_PKT_STREAM_CLOSE;
    packet.header.stream_id = 1;

    assert(vr_protocol_handle_packet(&conn, &packet) == NULL);
    assert(conn.active_streams == 1);
    assert(conn.streams_bitmap[0] == 3);
}

int main(void)
{
    assert(VR_HDR_SIZE == 8);
    test_serialization_and_types();
    test_one_byte_header_reads();
    test_empty_input_and_fragmented_payload();
    test_maximum_payload_length();
    test_multiple_packets_and_partial_header();
    test_invalid_header();
    test_wrapped_header_and_partial_payload();
    test_stream_close_has_no_undefined_response();
    test_duplicate_stream_close_is_ignored();
    test_unopened_stream_close_is_ignored();
    test_default_stream_close_is_ignored();
    test_stream_close_with_one_active_stream_is_ignored();
    return 0;
}