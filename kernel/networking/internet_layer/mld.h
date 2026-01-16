#pragma once

#include "types.h"
#include "net/network_types.h"
#include "networking/netpkt.h"

#ifdef __cplusplus
extern "C" {
#endif

bool mld_send_join(uint8_t ifindex, const uint8_t group[16]);
bool mld_send_leave(uint8_t ifindex, const uint8_t group[16]);
void mld_input(uint8_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], const void* l4, uint32_t l4_len);

#ifdef __cplusplus
}
#endif
