#include "velora/parser.h"

//until completely received.. give VR_EMPTY
vr_result_t vr_parser_poll(vr_parser_t *parser, vr_connection_t *conn, vr_packet_t *out)
{
    if (parser->state == VR_PARSER_HEADER_WAIT)
    {
        if (vr_conn_ring_buf_size(&conn->read_buf) < sizeof(vr_packet_header_t))
            return VR_EMPTY;

        //consumer.. fills in order
        uint8_t *header_ptr = (uint8_t *)&parser->current_header;
        for (uint_fast8_t i = 0; i < sizeof(vr_packet_header_t); i++)
        {
            if (vr_conn_ring_buf_pop(&conn->read_buf, &header_ptr[i]) == VR_ERROR)
                return VR_ERROR;
        }
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
        parser->state = VR_PARSER_PAYLOAD_WAIT;
        return VR_EMPTY;
    }
    else 
    {
        return VR_SUCCESS;
    }
}