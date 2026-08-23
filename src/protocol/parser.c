#include "velora/parser.h"
#include "velora/packet.h"
#include "velora/conn.h"
//until completely received.. give VR_EMPTY
vr_result_t vr_parser_poll(vr_parser_t *parser, vr_connection_t *conn, vr_packet_t *out)
{
    if (parser->state == VR_PARSER_HEADER_WAIT)
    {
        if (vr_conn_ring_buf_size(&conn->read_buf) < sizeof(vr_packet_header_t))
            return VR_EMPTY;

        uint8_t *contig = NULL;
        uint32_t contig_len = vr_conn_ring_buf_contiguous_read(&conn->read_buf, &contig);

        if (contig_len >= sizeof(vr_packet_header_t))
        {
            // Fast path: header lies fully within one contiguous region,
            // deserialize straight from the ring, no copy.
            vr_packet_header_deserialize(&parser->current_header, contig);
        }
        else
        {
            // Wrap path: header straddles the physical end of the ring,
            // flatten it into a small stack buffer first.
            uint8_t header_bytes[sizeof(vr_packet_header_t)];
            if (vr_conn_ring_buf_peek_n(&conn->read_buf, header_bytes, sizeof(vr_packet_header_t)) == VR_ERROR)
                return VR_ERROR;
            vr_packet_header_deserialize(&parser->current_header, header_bytes);
        }

        if (vr_conn_ring_buf_consume(&conn->read_buf, sizeof(vr_packet_header_t)) == VR_ERROR)
            return VR_ERROR;

        vr_log(VR_LOG_INFO, "header size: %zu", sizeof(vr_packet_header_t));
        vr_log(VR_LOG_INFO,
                "magic: %d, ver: %d, type: %d, stream: %d, flags: %d, payload_len: %d",
                parser->current_header.magic,
                parser->current_header.version,
                parser->current_header.type,
                parser->current_header.stream_id,
                parser->current_header.flags,
                parser->current_header.payload_len
        );
        //validation
        bool validated = true;
        if (parser->current_header.magic != VR_MAGIC)
            validated = false;
        if (parser->current_header.version != VR_PROTOCOL_VERSION)
            validated = false;
        if (parser->current_header.type > VR_PKT_ERROR)
            validated = false;
        if (parser->current_header.flags & ~VR_PACKET_VALID_FLAGS)
            validated = false;
        if (validated)
        {
            vr_log(VR_LOG_INFO, "Packet validated");
            if (parser->current_header.payload_len == 0)
            {
                out->header = parser->current_header;
                parser->state = VR_PARSER_HEADER_WAIT;
                memset(&(parser->current_header), 0, sizeof(parser->current_header));
                return VR_SUCCESS;
            }
        }
        else
        {
            vr_log(VR_LOG_WARN, "Packet not validatded");
            memset(&(parser->current_header), 0, sizeof(parser->current_header));
            parser->state = VR_PARSER_HEADER_WAIT;
            return VR_ERROR;
        }
        parser->state = VR_PARSER_PAYLOAD_WAIT;
    }

    if (parser->state == VR_PARSER_PAYLOAD_WAIT)
    {
        uint32_t payload_len = parser->current_header.payload_len;

        if (vr_conn_ring_buf_size(&conn->read_buf) < payload_len)
            return VR_EMPTY;
        out->payload = malloc(payload_len);
        if (out->payload == NULL)
        {
            vr_perror("Allocation error while parsing payload");
            return VR_ERROR;
        }

        uint8_t *contig = NULL;
        uint32_t contig_len = vr_conn_ring_buf_contiguous_read(&conn->read_buf, &contig);

        if (contig_len >= payload_len)
        {
            // Fast path: whole payload is one contiguous span, single memcpy.
            memcpy(out->payload, contig, payload_len);
        }
        else
        {
            // Wrap path: payload straddles the physical end of the ring,
            // flatten it with at most two memcpy()s via peek_n().
            if (vr_conn_ring_buf_peek_n(&conn->read_buf, out->payload, payload_len) == VR_ERROR)
            {
                free(out->payload);
                out->payload = NULL;
                return VR_ERROR;
            }
        }

        if (vr_conn_ring_buf_consume(&conn->read_buf, payload_len) == VR_ERROR)
        {
            free(out->payload);
            out->payload = NULL;
            return VR_ERROR;
        }

        vr_log(VR_LOG_INFO, "Payload: %u", out->payload);
        out->header = parser->current_header;
        memset(&(parser->current_header), 0, sizeof(parser->current_header));
        parser->state = VR_PARSER_HEADER_WAIT;
    }
    return VR_SUCCESS;
}