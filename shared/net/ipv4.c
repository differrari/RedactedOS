#include "ipv4.h"
#include "console/kio.h"
#include "network_types.h"
#include "std/string.h"
#include "std/memfunctions.h"

uintptr_t create_ipv4_packet(uintptr_t p, uint32_t payload_len, uint8_t protocol, uint32_t source_ip, uint32_t destination_ip){
    ipv4_hdr_t* ip = (ipv4_hdr_t*)p;
    ip->version_ihl = 0x45;
    ip->dscp_ecn = 0;
    ip->total_length = __builtin_bswap16(sizeof(ipv4_hdr_t) + payload_len);
    ip->identification = 0;
    ip->flags_frag_offset = __builtin_bswap16(0x4000);
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->src_ip = __builtin_bswap32(source_ip);
    ip->dst_ip = __builtin_bswap32(destination_ip);
    ip->header_checksum = checksum16((uint16_t*)ip, 10);
    return p + sizeof(ipv4_hdr_t);
}

uint8_t ipv4_get_protocol(uintptr_t ptr){
    return ((ipv4_hdr_t*)ptr)->protocol;
}

void ipv4_populate_response(network_connection_ctx *ctx, eth_hdr_t *eth, ipv4_hdr_t* ipv4){
    ctx->ip = __builtin_bswap32(ipv4->src_ip);
    memcpy(ctx->mac, eth->src_mac, 6);
}

string ipv4_to_string(uint32_t ip){
    return string_format("%i.%i.%i.%i",(ip >> 24) & 0xFF,(ip >> 16) & 0xFF,(ip >> 8) & 0xFF,(ip >> 0) & 0xFF);
}

uint32_t ipv4_get_source(uintptr_t ptr){
    return __builtin_bswap32(((ipv4_hdr_t*)ptr)->src_ip);
}