#include "eth.h"
#include "std/memfunctions.h"

uintptr_t create_eth_packet(uint8_t* buf, uint8_t src_mac[6], uint8_t dst_mac[6], uint16_t type){
    uintptr_t p = (uintptr_t)buf;

    eth_hdr_t* eth = (eth_hdr_t*)p;
    memcpy(eth->src_mac, src_mac, 6);
    memcpy(eth->dst_mac, dst_mac, 6);
    eth->ethertype = __builtin_bswap16(type);
    return p + sizeof(eth_hdr_t);
}

uint16_t eth_parse_packet_type(uintptr_t ptr){
    eth_hdr_t* eth = (eth_hdr_t*)ptr;
    
    ptr += sizeof(eth_hdr_t);
    
    return __builtin_bswap16(eth->ethertype);
}