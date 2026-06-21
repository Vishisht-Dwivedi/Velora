#ifndef VR_PARSER_H
#define VR_PARSER_H

#include "velora/common.h"
#include "velora/packet.h"
#include "velora/logger.h"
typedef struct vr_connection vr_connection_t;
typedef enum
{
    VR_PARSER_HEADER_WAIT,
    VR_PARSER_PAYLOAD_WAIT
} vr_parser_state_t;

typedef struct
{
    vr_parser_state_t state;
    vr_packet_header_t current_header;
} vr_parser_t;


uint8_t *vr_parser_encode(vr_packet_t *packet);
vr_result_t vr_parser_poll(vr_parser_t *parser, vr_connection_t *conn, vr_packet_t *out);

#endif