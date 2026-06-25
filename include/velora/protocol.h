#ifndef VR_PROTOCOL_H
#define VR_PROTOCOL_H
#include "velora/common.h"
#include "velora/packet.h"

typedef enum
{
    VR_PROTO_INIT, //default state
    VR_PROTO_CONNECTING, // for client.. to keep track of sending connect state
    VR_PROTO_ESTABILISHED, // for server/client.. after ack or after sendng ack.. its est
    VR_PROTO_CLOSED
} vr_protocol_state_t;
\
#include "velora/conn.h"
vr_packet_t *vr_protocol_handle_packet(vr_connection_t *conn, vr_packet_t *pkt);

#endif