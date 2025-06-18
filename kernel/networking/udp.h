#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "packets.h"

void create_udp_packet(uint8_t* buf, network_connection_ctx source, network_connection_ctx destination, const uint8_t* payload, uint16_t payload_len);
size_t calc_udp_packet_size(uint16_t payload_len);
void udp_parse_packet(uintptr_t packet_ptr);

#ifdef __cplusplus
}
#endif