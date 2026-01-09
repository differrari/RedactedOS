#pragma once

#include "types.h"
#include "net/network_types.h"
#include "networking/netpkt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __attribute__((packed)) {
    uint32_t ver_tc_fl;
    uint16_t payload_len;
    uint8_t next_header;
    uint8_t hop_limit;
    uint8_t src[16];
    uint8_t dst[16];
} ipv6_hdr_t;

typedef ip_tx_scope_t ipv6_tx_scope_t;
typedef ip_tx_opts_t ipv6_tx_opts_t;

void ipv6_send_packet(const uint8_t dst[16], uint8_t next_header, netpkt_t* pkt, const ipv6_tx_opts_t* opts, uint8_t hop_limit, uint8_t dontfrag);
void ipv6_input(uint16_t ifindex, netpkt_t* pkt, const uint8_t src_mac[6]);

uint16_t ipv6_pmtu_get(const uint8_t dst[16]);
void ipv6_pmtu_note(const uint8_t dst[16], uint16_t mtu);


#ifdef __cplusplus
}
#endif