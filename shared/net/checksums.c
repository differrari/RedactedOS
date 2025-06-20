#include "network_types.h"

uint16_t checksum16(uint16_t *data, size_t len) {
    uint32_t sum = 0;
    for (int i = 0; i < len; i++) sum += data[i];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum;
}

uint16_t checksum16_pipv4(
    uint32_t src_ip,
    uint32_t dst_ip,
    uint8_t protocol,
    const uint8_t* udp_header_and_payload,
    uint16_t length
) {
    uint32_t sum = 0;

    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += protocol;
    sum += length;

    for (uint16_t i = 0; i + 1 < length; i += 2)
        sum += (udp_header_and_payload[i] << 8) | udp_header_and_payload[i + 1];

    if (length & 1)
        sum += udp_header_and_payload[length - 1] << 8;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return ~sum;
}