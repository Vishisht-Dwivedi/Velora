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

#include "velora/conn.h"
vr_packet_t *vr_protocol_handle_packet(vr_connection_t *conn, vr_packet_t *pkt);

//FSM dictates state mapping... action tracks what should the parser do...
typedef struct
{
	bool send;
	vr_packet_type_t type;
	uint8_t stream_id;
	vr_packet_flag_t flags;
	const uint8_t *payload;
	uint16_t payload_len;
} vr_protocol_action_t;

#endif
