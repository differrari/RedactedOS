#pragma once
#include "types.h"
#include "std/string.h"
#include "networking/link_layer/eth.h"
#include "net/network_types.h"
#include "net/checksums.h"
#include "networking/interface_manager.h"
#include "networking/netpkt.h"

#define IP_IHL_NOOPTS 5
#define IP_VERSION_4 4
#define IP_TTL_DEFAULT 64

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __attribute__((packed)) ipv4_hdr_t {
    uint8_t version_ihl;
    uint8_t dscp_ecn;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_frag_offset;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t header_checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} ipv4_hdr_t;

typedef ip_tx_scope_t ipv4_tx_scope_t;
typedef ip_tx_opts_t ipv4_tx_opts_t;

void ipv4_send_packet(uint32_t dst_ip, uint8_t proto, netpkt_t* pkt, const ipv4_tx_opts_t* opts, uint8_t ttl, uint8_t dontfrag);
void ipv4_input(uint16_t ifindex, netpkt_t* pkt, const uint8_t src_mac[6]);

#ifdef __cplusplus
}
#endif
