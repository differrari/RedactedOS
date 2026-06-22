#include "eth.h"
#include "std/memory.h"
#include "networking/network.h"
#include "arp.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv6.h"
#include "console/kio.h"
#include "syscalls/syscalls.h"
uintptr_t create_eth_packet(uintptr_t p, const uint8_t src_mac[MAC_ADDR_LEN], const uint8_t dst_mac[MAC_ADDR_LEN], uint16_t type) {
    uint8_t* eth = (uint8_t*)p;

    mac_copy(eth, dst_mac);
    mac_copy(eth + MAC_ADDR_LEN, src_mac);
    wr_be16(eth+12, type);

    return p + (uint32_t)sizeof(eth_hdr_t);
}

uint16_t eth_parse_type(const netpkt_t* pkt){
    uint16_t type = 0;
    if (!pkt || !netpkt_copyout(pkt, 12u, &type, sizeof(type))) return 0;
    return rd_be16(&type);
}

bool eth_src(const netpkt_t* pkt, uint8_t out[MAC_ADDR_LEN]){
    if (!pkt || !out) return false;
    return netpkt_copyout(pkt, 6u, out, MAC_ADDR_LEN);
}

bool eth_dst(const netpkt_t* pkt, uint8_t out[MAC_ADDR_LEN]){
    if (!pkt || !out) return false;
    return netpkt_copyout(pkt, 0u, out, MAC_ADDR_LEN);
}

bool eth_send_frame_on(uint16_t ifindex, uint16_t ethertype, const uint8_t dst_mac[MAC_ADDR_LEN], netpkt_t* pkt){
    const uint8_t* src_mac = network_get_mac(ifindex);
    if (!src_mac || !dst_mac || !pkt) {
        if (pkt) netpkt_unref(pkt);
        return false;
    }

    uint32_t driver_headroom = network_get_header_size(ifindex);
    if (!netpkt_ensure_headroom(pkt, (uint32_t)sizeof(eth_hdr_t) + driver_headroom)) {
        netpkt_unref(pkt);
        return false;
    }

    void* hdrp = netpkt_push(pkt, (uint32_t)sizeof(eth_hdr_t));
    if (!hdrp) {
        netpkt_unref(pkt);
        return false;
    }

    (void)create_eth_packet((uintptr_t)hdrp, src_mac, dst_mac, ethertype);

    bool ok = (net_tx_packet_on(ifindex, pkt) == 0);
    if (!ok) netpkt_unref(pkt);
    return ok;
}

void eth_input(uint16_t ifindex, netpkt_t* pkt) {
    if (!pkt) return;

    if (netpkt_len(pkt) < sizeof(eth_hdr_t)) return;
    eth_hdr_t eth;
    if (!netpkt_copyout(pkt, 0, &eth, sizeof(eth))) return;
    if (!netpkt_pull(pkt, (uint32_t)sizeof(eth_hdr_t))) return;

    uint16_t type = bswap16(eth.ethertype);

    switch (type) {
        case ETHERTYPE_ARP:
            arp_input(ifindex, eth.src_mac, pkt);
            break;
        case ETHERTYPE_IPV4:
            ipv4_input(ifindex, pkt, eth.src_mac);
            break;
        case ETHERTYPE_IPV6:
            ipv6_input(ifindex, pkt, eth.src_mac);
            break;
        case ETHERTYPE_VLAN1Q:
            break;
        case ETHERTYPE_VLAN1AD:
            break;
        default:
            break;
    }
}
