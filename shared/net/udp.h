#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "net/network_types.h"

typedef struct __attribute__((packed)) udp_hdr_t {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_hdr_t;

void create_udp_packet(uint8_t* buf, network_connection_ctx source, network_connection_ctx destination, const uint8_t* payload, uint16_t payload_len);
size_t calc_udp_size(uint16_t payload_len);
uint16_t udp_parse_packet(uintptr_t ptr);
sizedptr udp_parse_packet_payload(uintptr_t ptr);
uint16_t udp_parse_packet_length(uintptr_t ptr);

#ifdef __cplusplus
}
#endif