#include "eth.h"
#include "std/memory.h"
#include "networking/network.h"
#include "arp.h"
#include "networking/internet_layer/ipv4.h"
//#include "networking/internet_layer/ipv6.h"
#include "console/kio.h"
#include "syscalls/syscalls.h"


uintptr_t create_eth_packet(uintptr_t p,
                            const uint8_t src_mac[6],
                            const uint8_t dst_mac[6],
                            uint16_t type)
{
    eth_hdr_t* eth =(eth_hdr_t*)p;

    memcpy(eth->dst_mac, dst_mac, 6);
    memcpy(eth->src_mac, src_mac, 6);
    eth->ethertype = bswap16(type);

    return p + sizeof(eth_hdr_t);
}

uint16_t eth_parse_type(uintptr_t ptr){
    const eth_hdr_t* eth = (const eth_hdr_t*)ptr;
    return bswap16(eth->ethertype);
}

const uint8_t* eth_src(uintptr_t ptr){
    const eth_hdr_t* eth = (const eth_hdr_t*)ptr;
    return eth->src_mac;
}

const uint8_t* eth_dst(uintptr_t ptr){
    const eth_hdr_t* eth = (const eth_hdr_t*)ptr;
    return eth->dst_mac;
}

bool eth_send_frame_on(uint16_t ifindex, uint16_t ethertype, const uint8_t dst_mac[6], sizedptr payload){
    const uint8_t* src_mac = network_get_mac(ifindex);
    if (!src_mac || !dst_mac) return false;

    uint32_t total = (uint32_t)sizeof(eth_hdr_t) + (uint32_t)payload.size;
    uintptr_t buf = (uintptr_t)malloc(total);
    if (!buf) return false;

    uintptr_t ptr = create_eth_packet(buf, src_mac, dst_mac, ethertype);

    if (payload.size) memcpy((void*)ptr, (const void*)payload.ptr, payload.size);

    bool ok = (net_tx_frame_on(ifindex, buf, total) == 0);
    free_sized((void*)buf, total);
    return ok;
}

void eth_input(uint16_t ifindex, uintptr_t frame_ptr, uint32_t frame_len){

    if (frame_len < sizeof(eth_hdr_t)) return;

    uint16_t type = eth_parse_type(frame_ptr);
    const uint8_t* src_mac = eth_src(frame_ptr);
    uintptr_t payload_ptr = frame_ptr + sizeof(eth_hdr_t);
    uint32_t payload_len = frame_len - (uint32_t)sizeof(eth_hdr_t);
    switch (type) {
        case ETHERTYPE_ARP:
            arp_input(ifindex, frame_ptr, frame_len);
            break;
        case ETHERTYPE_IPV4:
            ipv4_input(ifindex, payload_ptr, payload_len, src_mac);
            break;
        case ETHERTYPE_IPV6: //TODO IPV6
            break;
        case ETHERTYPE_VLAN1Q: //TODO vlan
            break;
        case ETHERTYPE_VLAN1AD: //TODO vlan
            break;
        default:
            break;
    }
}
