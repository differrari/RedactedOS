#pragma once
#include "types.h"
#include "net/network_types.h"
#include "networking/netpkt.h"
#include "networking/link_layer/link_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_VLAN1Q 0x8100
#define ETHERTYPE_VLAN1AD 0x88A8
#define ETHERTYPE_IPV6 0x86DD

typedef struct __attribute__((packed)) eth_hdr_t {
    uint8_t dst_mac[MAC_ADDR_LEN];
    uint8_t src_mac[MAC_ADDR_LEN];
    uint16_t ethertype;
} eth_hdr_t;

uint16_t eth_parse_type(const netpkt_t* pkt);
bool eth_src(const netpkt_t* pkt, uint8_t out[MAC_ADDR_LEN]);
bool eth_dst(const netpkt_t* pkt, uint8_t out[MAC_ADDR_LEN]);

bool eth_send_frame_on(uint8_t ifindex, uint16_t ethertype, const uint8_t dst_mac[MAC_ADDR_LEN], netpkt_t* pkt);

void eth_input(uint8_t ifindex, netpkt_t* pkt);

#ifdef __cplusplus
}
#endif
