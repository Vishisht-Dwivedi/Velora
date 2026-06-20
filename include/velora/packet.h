#ifndef VR_PACKET_H
#define VR_PACKET_H
#include "velora/common.h"
#include "velora/conn.h"

typedef enum
{
    VR_PKT_CONNNECT = 1, // trying to connect
    VR_PKT_CONNECT_ACK, // ack on the connect packet
    VR_PKT_PING, // checkup
    VR_PKT_PONG, // ack to checkup
    VR_PKT_STREAM_OPEN, // declaring startup of stream
    VR_PKT_STREAM_CLOSE, // declaring closing of stream
    VR_PKT_PUBLISH, // packet
    VR_PKT_ACK // ack to packet
} vr_packet_type_t;

typedef enum
{
    VR_FLAG_NONE = 0,
    VR_FLAG_COMPRESSED = 1 << 0,
    VR_FLAG_ACK_REQ = 1 << 1
} vr_packet_flag_t;

typedef struct
{
    uint16_t magic; // identifying packet to be that of velora
    uint8_t version;
    uint8_t type; // type of packet...ex ping ack rpc etc
    uint8_t stream_id; //256 max streams multiplexed into one conn
    uint8_t flags;
    uint16_t payload_len; // max allowed 65535 bytes 
} vr_packet_header_t;

#endif