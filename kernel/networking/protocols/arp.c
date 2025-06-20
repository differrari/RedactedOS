#include "arp.h"
#include "std/memfunctions.h"

void create_arp_packet(uint8_t* buf, uint8_t* src_mac, uint32_t src_ip, uint8_t* dst_mac, uint32_t dst_ip, bool is_request){
    uintptr_t p = (uintptr_t)buf;

    eth_hdr_t* eth = (eth_hdr_t*)p;
    for (int i = 0; i < 6; i++) eth->dst_mac[i] = is_request ? 0xFF : dst_mac[i];
    memcpy(eth->src_mac, src_mac, 6);
    eth->ethertype = __builtin_bswap16(0x0806);
    p += sizeof(eth_hdr_t);
    
    arp_hdr_t* arp = (arp_hdr_t*)p;

    arp->htype = 1;
    arp->ptype = __builtin_bswap16(0x0800);
    arp->hlen = 6;
    arp->plen = 4;
    arp->opcode = __builtin_bswap16(is_request ? 1 : 2);
    memcpy(arp->sender_mac, src_mac, 6);
    arp->sender_ip = src_ip;
    memcpy(arp->target_mac, dst_mac, 6);
    arp->target_ip = dst_ip;
}

bool arp_should_handle(uintptr_t ptr, uint32_t ip){
    arp_hdr_t* arp = (arp_hdr_t*)ptr;

    return arp->target_ip == ip;
}