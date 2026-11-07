#pragma once

#include "types.h"
#include "net/network_types.h"
#include "networking/netpkt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IPV6_NH_HOP_BY_HOP = 0,
    IPV6_NH_ROUTING = 43,
    IPV6_NH_FRAGMENT = 44,
    IPV6_NH_ESP = 50,
    IPV6_NH_AH = 51,
    IPV6_NH_NO_NEXT = 59,
    IPV6_NH_DEST_OPTS = 60
} ipv6_next_header_t;

typedef struct __attribute__((packed)) {
    uint32_t ver_tc_fl;
    uint16_t payload_len;
    uint8_t next_header;
    uint8_t hop_limit;
    uint8_t src[16];
    uint8_t dst[16];
} ipv6_hdr_t;

bool ipv6_send_packet(const uint8_t dst[16], uint8_t next_header, netpkt_t* pkt, const ip_tx_opts_t* opts, uint8_t hop_limit, uint8_t dontfrag, uint8_t dontroute);
bool ipv6_skip_ext_headers(const netpkt_t* pkt, uint8_t* nh, uint32_t* l4_off, uint32_t* l4_len, bool stop_at_fragment, bool* router_alert);
void ipv6_input(uint8_t ifindex, netpkt_t* pkt, const uint8_t src_mac[6]);

#ifdef __cplusplus
}
#endif