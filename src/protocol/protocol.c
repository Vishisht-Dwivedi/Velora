#include "velora/protocol.h"
#include <stdint.h>
#include <limits.h>
//server side
//handle close in main loop since manager exists there
vr_packet_t *vr_protocol_handle_packet(vr_connection_t *conn, vr_packet_t *pkt)
{
    if (pkt == NULL || conn == NULL)
    {
        vr_log(VR_LOG_ERROR, "Null packet or conn object passed to handler");
        return NULL;
    }

    vr_packet_t *response = NULL;

    switch (conn->proto_state)
    {
        case VR_PROTO_INIT:
        {
            if (pkt->header.type == VR_PKT_CONNECT) 
            {
                response = malloc(sizeof(vr_packet_t));
                response->header.flags = VR_FLAG_NONE;
                response->header.magic = VR_MAGIC;
                response->header.payload_len = 0;
                response->header.stream_id = 0; //0-255 streams.. 0 is default
                response->header.type = VR_PKT_CONNECT_ACK;
                response->header.version = VR_PROTOCOL_VERSION;
                conn->streams_bitmap[0] |= 1;
                conn->active_streams = 1;
                conn->proto_state = VR_PROTO_ESTABILISHED;
            }
            break;
        }
        case VR_PROTO_CONNECTING:
        {
            if (pkt->header.type == VR_PKT_CONNECT_ACK) 
            {
                conn->proto_state = VR_PROTO_ESTABILISHED;
            }
            break;
        }
        case VR_PROTO_ESTABILISHED:
        {
            response = malloc(sizeof(vr_packet_t));
            response->header.flags = VR_FLAG_NONE;
            response->header.magic = VR_MAGIC;
            response->header.stream_id = pkt->header.stream_id;
            response->header.version = VR_PROTOCOL_VERSION;
            switch (pkt->header.type)
            {
                case VR_PKT_PING: {
                    response->header.type = VR_PKT_PONG;
                    response->header.payload_len = 0;
                    break;
                }
                case VR_PKT_STREAM_OPEN: {
                    uint64_t idx = -1;
                    uint8_t offset = 0;
                    for (offset = 0; offset < 4; offset++)
                    {
                        if (conn->streams_bitmap[offset] == ULLONG_MAX)
                            continue;
                        idx = __builtin_ctzll(~(conn->streams_bitmap[offset]));
                        break;
                    }
                    if (idx == -1) 
                    {
                        response->header.type = VR_PKT_ERROR;
                        response->payload = 0;
                    }
                    else 
                    {
                        conn->streams_bitmap[offset] |= 1ULL << idx;
                        conn->active_streams++;
                        response->header.type = VR_PKT_STREAM_OPEN_ACK;
                        response->header.stream_id = 64 * offset + idx;
                        response->header.payload_len = 0;
                    }
                    break;
                }
                case VR_PKT_STREAM_CLOSE: {
                    uint8_t bitmap_idx = pkt->header.stream_id / 64;
                    uint8_t bitmap_offset = pkt->header.stream_id % 64;
                    uint64_t bit_neg = 1ULL << bitmap_offset;
                    conn->streams_bitmap[bitmap_idx] &= ~(bit_neg);
                    conn->active_streams--;
                    if (conn->active_streams == 0)
                    {
                        free(response);
                        response = NULL;
                        conn->proto_state = VR_PROTO_CLOSED;
                    }
                    break;
                }
                case VR_PKT_PUBLISH: {
                    free(response);
                    response = NULL;
                    break;
                }
                case VR_PKT_ERROR: {
                    free(response);
                    response = NULL;
                    break;
                }
                default: {
                    response->header.type = VR_PKT_ERROR;
                    response->header.payload_len = 0;
                }
            }
            break;
        }
        default: 
        {
            vr_log(VR_LOG_ERROR, "Invalid state");
            conn->proto_state = VR_PROTO_INIT;
            free(response);
            return NULL;
        }
    }
    if (response != NULL)
    {
        vr_log(VR_LOG_INFO,
               "Response packet: magic: %u, version %u, type: %u, stream_id: %u, flags: %u, payload_len: %u",
               response->header.magic,
               response->header.version,
               response->header.type,
               response->header.stream_id,
               response->header.flags,
               response->header.payload_len);
    }
    return response;
}