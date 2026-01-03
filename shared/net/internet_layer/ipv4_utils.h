#pragma once
#include "types.h"
#define IPV4_MCAST_ALL_HOSTS 0xE0000001u
#define IPV4_MCAST_ALL_ROUTERS 0xE0000002u

#ifdef __cplusplus
extern "C" {
#endif

bool ipv4_is_unspecified(uint32_t ip);
bool ipv4_is_loopback(uint32_t ip);
bool ipv4_is_multicast(uint32_t ip);
bool ipv4_is_link_local(uint32_t ip);
bool ipv4_is_private(uint32_t ip);
bool ipv4_is_cgnat(uint32_t ip);
bool ipv4_is_documentation(uint32_t ip);
bool ipv4_is_benchmark(uint32_t ip);
bool ipv4_is_reserved(uint32_t ip);
bool ipv4_is_reserved_special(uint32_t ip);
bool ipv4_is_unicast_global(uint32_t ip);

bool ipv4_mask_is_contiguous(uint32_t mask);
int ipv4_prefix_len(uint32_t mask);

uint32_t ipv4_net(uint32_t ip, uint32_t mask);
uint32_t ipv4_broadcast_calc(uint32_t ip, uint32_t mask);

bool ipv4_is_network_address(uint32_t ip, uint32_t mask);
bool ipv4_is_broadcast_address(uint32_t ip, uint32_t mask);
bool ipv4_is_limited_broadcast(uint32_t ip);
bool ipv4_is_directed_broadcast(uint32_t ip, uint32_t mask, uint32_t dst);
bool ipv4_same_subnet(uint32_t a, uint32_t b, uint32_t mask);

void ipv4_to_string(uint32_t ip, char* buf);
bool ipv4_parse(const char* s, uint32_t* out);

void ipv4_mcast_to_mac(uint32_t group, uint8_t out_mac[6]);

#ifdef __cplusplus
}
#endif