#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void ipv6_addr_manager_on_ra(uint8_t ifindex, const uint8_t router_ip[16], uint16_t router_lifetime, const uint8_t prefix[16],uint8_t prefix_len, uint32_t valid_lft, uint32_t preferred_lft, uint8_t autonomous);
int ipv6_addr_manager_daemon_entry(int argc, char* argv[]);

#ifdef __cplusplus
}
#endif