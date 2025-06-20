#pragma once

#ifdef __cplusplus
extern "C" {
#endif
    
#include "types.h"

typedef enum NetProtocol {
    UDP,
    DHCP,
    ARP,
    TCP
} NetProtocol;

typedef struct __attribute__((packed)) eth_hdr_t {
    uint8_t dst_mac[6];
    uint8_t src_mac[6];
    uint16_t ethertype;
} eth_hdr_t;

typedef struct __attribute__((packed)) ipv4_hdr_t {
    uint8_t  version_ihl;
    uint8_t  dscp_ecn;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_frag_offset;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t header_checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} ipv4_hdr_t;

typedef struct network_connection_ctx {
    uint16_t port;
    uint32_t ip;
    uint8_t mac[6];
} network_connection_ctx;

#ifdef __cplusplus
}
#endif