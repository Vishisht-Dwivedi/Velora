#include "velora/packet.h"

void vr_encode_u16(uint8_t *buf, uint16_t value)
{
    // 0x5789... u take 8 bits out to get 0x57.. and with 0xFF(all set).. 
    // since u dealt with numbers.. it doesnt care abt endianness..
    buf[0] = (value >> 8) & 0xFF;
    //just & with last 8 bits
    buf[1] = value & 0xFF;
}

uint16_t vr_decode_u16(uint8_t *buf)
{
    // just take last byte.. make it into 2 byte long.. then shift it on the scale..
    // then just or the remaining byte..
    return ((uint16_t)(buf[0] << 8) | (uint16_t)(buf[1]));
}
// ensure that buffer is big enough to hold payload on caller's side
void vr_packet_serialize(vr_packet_t *packet, uint8_t *buff)
{
    if (packet == NULL) 
    {
        vr_log(VR_LOG_ERROR, "Empty packet sent to serializer");
        return;
    }
    uint8_t *p = buff;
    vr_encode_u16(p, packet->header.magic);
    p += 2;
    *p++ = packet->header.version;
    *p++ = packet->header.type;
    *p++ = packet->header.stream_id;
    *p++ = packet->header.flags;
    vr_encode_u16(p, packet->header.payload_len);
    p += 2;
    if (packet->header.payload_len > 0)
        memcpy(p, packet->payload, packet->header.payload_len);
}

void vr_packet_header_deserialize(vr_packet_header_t *header, uint8_t *data)
{
    uint8_t *p = data;
    header->magic = vr_decode_u16(p);
    p += 2;
    header->version = *p++;
    header->type = *p++;
    header->stream_id = *p++;
    header->flags = *p++;
    header->payload_len = vr_decode_u16(p);
}
