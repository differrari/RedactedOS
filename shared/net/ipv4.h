#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "net/network_types.h"

uint8_t ipv4_get_protocol(uintptr_t ptr);
uintptr_t create_ipv4_packet(uintptr_t p, uint32_t payload_len, uint8_t protocol, uint32_t source_ip, uint32_t destination_ip);

#ifdef __cplusplus
}
#endif