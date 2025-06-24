#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "net/network_types.h"
#include "std/string.h"
#include "eth.h"

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

uint8_t ipv4_get_protocol(uintptr_t ptr);
uintptr_t create_ipv4_packet(uintptr_t p, uint32_t payload_len, uint8_t protocol, uint32_t source_ip, uint32_t destination_ip);
void ipv4_populate_response(network_connection_ctx *ctx, eth_hdr_t *eth, ipv4_hdr_t* ipv4);
string ipv4_to_string(uint32_t ip);
uint32_t ipv4_get_source(uintptr_t ptr);

#ifdef __cplusplus
}
#endif