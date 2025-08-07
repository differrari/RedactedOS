#include "checksums.h"

uint16_t checksum16(const uint16_t *data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t checksum16_pipv4(uint32_t src_ip,
                        uint32_t dst_ip,
                        uint8_t  protocol,
                        const uint8_t *payload,
                        uint16_t length)
{
    uint32_t sum = 0;

    sum += (src_ip >> 16) & 0xFFFF;
    sum +=  src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum +=  dst_ip & 0xFFFF;
    sum += protocol;
    sum += length;

    for (uint16_t i = 0; i + 1 < length; i += 2)
        sum += (payload[i] << 8) | payload[i + 1];

    if (length & 1)
        sum += payload[length - 1] << 8;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}
