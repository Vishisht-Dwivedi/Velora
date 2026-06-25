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

// allocates memory for payload after reading.. ensure its null earlier
void vr_packet_deserialize(vr_packet_t *packet, uint8_t *data)
{
    if (packet == NULL || data == NULL)
    {
        vr_log(VR_LOG_ERROR, "Null ptrs passed to deserializer");
        return;
    }
    uint8_t *p = data;
    packet->header.magic = vr_decode_u16(p);
    p += 2;
    packet->header.version = *p++;
    packet->header.type = *p++;
    packet->header.stream_id = *p++;
    packet->header.flags = *p++;
    packet->header.payload_len = vr_decode_u16(p);
    p += 2;
    if (packet->header.payload_len > 0)
    {
        packet->payload = malloc(packet->header.payload_len);
        if(packet->payload == NULL)
        {
            vr_perror("Memory allocation failed during deserialization");
            return;
        }
        memcpy(packet->payload, p, packet->header.payload_len);
    }
    else 
    {
        packet->payload = NULL;
    }
}