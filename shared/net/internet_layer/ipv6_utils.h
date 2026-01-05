#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IPV6_MCAST_SOLICITED_NODE = 0,
    IPV6_MCAST_ALL_NODES= 1,
    IPV6_MCAST_ALL_ROUTERS = 2,
    IPV6_MCAST_DHCPV6_SERVERS = 3,
    IPV6_MCAST_SSDP = 4,
    IPV6_MCAST_MDNS = 5,
    IPV6_MCAST_MLDV2_ROUTERS = 6,
} ipv6_mcast_kind_t;

bool ipv6_is_unspecified(const uint8_t ip[16]);
bool ipv6_is_loopback(const uint8_t ip[16]);
bool ipv6_is_multicast(const uint8_t ip[16]);
bool ipv6_is_ula(const uint8_t ip[16]);
bool ipv6_is_linklocal(const uint8_t ip[16]);
int ipv6_cmp(const uint8_t a[16], const uint8_t b[16]);
void ipv6_cpy(uint8_t dst[16], const uint8_t src[16]);
int ipv6_common_prefix_len(const uint8_t a[16], const uint8_t b[16]);
void ipv6_make_multicast(uint8_t scope, ipv6_mcast_kind_t kind, const uint8_t unicast[16], uint8_t out[16]);
void ipv6_to_string(const uint8_t ip[16], char* buf, int buflen);
bool ipv6_parse(const char* s, uint8_t out[16]);
void ipv6_multicast_mac(const uint8_t ip[16], uint8_t mac[6]);
void ipv6_make_lla_from_mac(uint8_t ifindex, uint8_t out[16]);

static inline int ipv6_is_placeholder_gua(const uint8_t ip[16]) {
    if (!ip) return 0;
    if (ip[0] != 0x20 || ip[1] != 0x00) return 0;
    for (int i = 2; i < 16; i++) if (ip[i] != 0) return 0;
    return 1;
}

static inline void ipv6_make_placeholder_gua(uint8_t out[16]) {
    if (!out) return;
    for (int i = 0; i < 16; i++) out[i] = 0;
    out[0] = 0x20;
    out[1] = 0x00;
}

static inline bool ipv6_is_linkscope_mcast(const uint8_t ip[16]){
    if (!ip) return false;
    if (ip[0] != 0xFF) return false;
    uint8_t scope = (uint8_t)(ip[1] & 0x0F);
    return scope == 0x02;
}

#ifdef __cplusplus
}
#endif
