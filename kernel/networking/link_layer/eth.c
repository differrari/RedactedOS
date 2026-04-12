#include "eth.h"
#include "std/memory.h"
#include "networking/network.h"
#include "arp.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv6.h"
#include "console/kio.h"
#include "syscalls/syscalls.h"
uintptr_t create_eth_packet(uintptr_t p, const uint8_t src_mac[6], const uint8_t dst_mac[6], uint16_t type) {
    uint8_t* eth = (uint8_t*)p;

    memcpy(eth, dst_mac, 6);
    memcpy(eth+6, src_mac, 6);
    wr_be16(eth+12, type);

    return p + (uint32_t)sizeof(eth_hdr_t);
}

uint16_t eth_parse_type(uintptr_t ptr){
    return rd_be16((const void*)(ptr+12));
}

const uint8_t* eth_src(uintptr_t ptr){
    return (const uint8_t*)(ptr+6);
}

const uint8_t* eth_dst(uintptr_t ptr){
    return (const uint8_t*)ptr;
}

bool eth_send_frame_on(uint16_t ifindex, uint16_t ethertype, const uint8_t dst_mac[6], netpkt_t* pkt){
    const uint8_t* src_mac = network_get_mac(ifindex);
    if (!src_mac || !dst_mac || !pkt) {
        if (pkt) netpkt_unref(pkt);
        return false;
    }

    void* hdrp = netpkt_push(pkt, (uint32_t)sizeof(eth_hdr_t));
    if (!hdrp) {
        netpkt_unref(pkt);
        return false;
    }

    (void)create_eth_packet((uintptr_t)hdrp, src_mac, dst_mac, ethertype);

    bool ok = (net_tx_frame_on(ifindex, netpkt_data(pkt), netpkt_len(pkt)) == 0);
    netpkt_unref(pkt);
    return ok;
}

void eth_input(uint16_t ifindex, netpkt_t* pkt) {
    if (!pkt) return;

    uint32_t frame_len = netpkt_len(pkt);
    uintptr_t frame_ptr = netpkt_data(pkt);
    if (frame_len < sizeof(eth_hdr_t)) return;

    uint16_t type = eth_parse_type(frame_ptr);
    const uint8_t* src_mac = eth_src(frame_ptr);

    switch (type) {
        case ETHERTYPE_ARP:
            arp_input(ifindex, pkt);
            break;
        case ETHERTYPE_IPV4:
            if (!netpkt_pull(pkt, (uint32_t)sizeof(eth_hdr_t))) break;
            ipv4_input(ifindex, pkt, src_mac);
            break;
        case ETHERTYPE_IPV6:
            if (!netpkt_pull(pkt, (uint32_t)sizeof(eth_hdr_t))) break;
            ipv6_input(ifindex, pkt, src_mac);
            break;
        case ETHERTYPE_VLAN1Q: //TODO vlan
            break;
        case ETHERTYPE_VLAN1AD: //TODO vlan
            break;
        default:
            break;
    }
}
