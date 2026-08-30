#pragma once

#include "types.h"
#include "net/network_types.h"
#include "networking/netpkt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ICMPV6_DEST_UNREACH = 1,
    ICMPV6_PACKET_TOO_BIG = 2,
    ICMPV6_TIME_EXCEEDED = 3,
    ICMPV6_PARAM_PROBLEM = 4,
    ICMPV6_ECHO_REQUEST = 128,
    ICMPV6_ECHO_REPLY = 129,
    ICMPV6_MLD_QUERY = 130,
    ICMPV6_MLD_REPORT = 131,
    ICMPV6_MLD_DONE = 132,
    ICMPV6_ROUTER_SOLICIT = 133,
    ICMPV6_ROUTER_ADVERT = 134,
    ICMPV6_NEIGHBOR_SOLICIT = 135,
    ICMPV6_NEIGHBOR_ADVERT = 136,
    ICMPV6_REDIRECT = 137,
    ICMPV6_MLDV2_REPORT = 143
} icmpv6_type_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
} icmpv6_hdr_t;

void icmpv6_input(uint16_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], uint8_t hop_limit, const uint8_t src_mac[6], netpkt_t* pkt);
bool icmpv6_send_on_l2(uint8_t ifindex, const uint8_t dst_ip[16], const uint8_t src_ip[16], const uint8_t dst_mac[6], const void *icmp, uint32_t icmp_len, uint8_t hop_limit);

#ifdef __cplusplus
}
#endif
