#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "network_types.h"

void create_udp_packet(uint8_t* buf, network_connection_ctx source, network_connection_ctx destination, const uint8_t* payload, uint16_t payload_len);
size_t calc_udp_size(uint16_t payload_len);
uint16_t udp_parse_packet(uintptr_t ptr);
sizedptr udp_parse_packet_payload(uintptr_t ptr);
uint16_t udp_parse_packet_length(uintptr_t ptr);

#ifdef __cplusplus
}
#endif