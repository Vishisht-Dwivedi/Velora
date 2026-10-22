#ifndef VR_PACKET_H
#define VR_PACKET_H
#include "velora/common.h"

#define VR_MAGIC 0x5789
#define VR_PROTOCOL_VERSION 1
#define VR_HDR_SIZE 8

typedef enum
{
    VR_PKT_CONNECT = 1, // trying to connect
    VR_PKT_CONNECT_ACK, // ack on the connect packet... no need for stream flow for default connection
    VR_PKT_PING, // checkup
    VR_PKT_PONG, // ack to checkup
    VR_PKT_STREAM_OPEN, // declaring startup of stream
    VR_PKT_STREAM_OPEN_ACK, //used to send stream info to client
    VR_PKT_STREAM_CLOSE, // declaring closing of stream
    VR_PKT_PUBLISH, // packet
    VR_PKT_ERROR // in case received packet was errornous
} vr_packet_type_t;

typedef enum
{
    VR_FLAG_NONE = 0,
    VR_FLAG_COMPRESSED = 1 << 0,
    VR_FLAG_ACK_REQ = 1 << 1
} vr_packet_flag_t;

#define VR_PACKET_VALID_FLAGS (VR_FLAG_COMPRESSED | VR_FLAG_ACK_REQ)

typedef struct
{
    uint16_t magic; // identifying packet to be that of velora
    uint8_t version;
    vr_packet_type_t type; // type of packet...ex ping ack rpc etc
    uint8_t stream_id; //256 max streams multiplexed into one conn
    uint8_t flags;
    uint16_t payload_len; // max allowed 65535 bytes
} vr_packet_header_t;

typedef struct
{
    vr_packet_header_t header;
    uint8_t *payload;
} vr_packet_t;

void vr_encode_u16(uint8_t *buf, uint16_t value);
uint16_t vr_decode_u16(uint8_t *buf);

void vr_packet_serialize(vr_packet_t *packet, uint8_t *buff);
void vr_packet_header_deserialize(vr_packet_header_t *header, uint8_t *data);

//view exists to separate the transport semantics from the parsing semantics
//this is all the info the packet parser must ever need... we dont care abt how
//this got here... we can replace underlying mechanism independently...
typedef struct
{
	const uint8_t *payload; //pointer to payload on the ring
	uint16_t payload_len; //size of payload
	vr_packet_type_t type; //same as vr_packet
	uint8_t stream_id; //multiplexing the connection
	uint8_t flags;
	uint64_t conn_id; //identifier for each conn... max 2^64.. enough conns
	uint32_t seq; //sequence number of current chunk
	uint32_t ack; //sequence number of current ack
	uint16_t recv_window; //parser got room for this much more data
} vr_packet_view_t;

#endif
