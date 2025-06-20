#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "net/network_types.h"

uint16_t eth_parse_packet_type(uintptr_t ptr);
uintptr_t create_eth_packet(uintptr_t ptr, uint8_t src_mac[6], uint8_t dst_mac[6], uint16_t type);

#ifdef __cplusplus
}
#endif