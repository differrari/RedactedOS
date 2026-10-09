#include "eth.h"
#include "networking/interface_manager.h"
#include "networking/network.h"
#include "arp.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv6.h"

uint16_t eth_parse_type(const netpkt_t* pkt){
    uint16_t type = 0;
    if (!pkt || !netpkt_copyout(pkt, 12u, &type, sizeof(type))) return 0;
    return rd_be16(&type);
}

bool eth_parse_vlan(const netpkt_t* pkt, uint16_t* vlan_id, uint16_t* inner_type) {
    uint8_t tag[ETH_VLAN_TAG_LEN];
    if (!pkt || !vlan_id || !inner_type || netpkt_len(pkt) < sizeof(eth_hdr_t) + sizeof(tag)) return false;
    if (!netpkt_copyout(pkt, sizeof(eth_hdr_t), tag, sizeof(tag))) return false;
    *vlan_id = rd_be16(tag) & 0x0FFF;
    *inner_type = rd_be16(tag + 2);
    return true;
}

bool eth_send_frame_on(uint8_t ifindex, uint16_t ethertype, const uint8_t dst_mac[MAC_ADDR_LEN], netpkt_t* pkt){
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!pkt || !itf || !itf->is_up || !dst_mac) {
        if (pkt) netpkt_unref(pkt);
        return false;
    }

    bool tagged = itf->link_kind == NET_LINK_VLAN;
    if (tagged) {
        l2_interface_t *parent = l2_interface_find_by_index(itf->parent_ifindex);
        if (!parent || parent->link_kind != NET_LINK_DIRECT || !parent->is_up) {
            netpkt_unref(pkt);
            return false;
        }
    }

    const uint8_t* src_mac = network_get_mac(ifindex);
    if (!src_mac) {
        netpkt_unref(pkt);
        return false;
    }
    uint32_t head_size = sizeof(eth_hdr_t) + (tagged ? ETH_VLAN_TAG_LEN : 0);
    uint32_t driver_headroom = network_get_header_size(ifindex);
    if (!netpkt_ensure_headroom(pkt, head_size + driver_headroom)) {
        netpkt_unref(pkt);
        return false;
    }

    uint8_t* hdrp = netpkt_push(pkt, head_size);
    if (!hdrp) {
        netpkt_unref(pkt);
        return false;
    }

    mac_copy(hdrp, dst_mac);
    mac_copy(hdrp + MAC_ADDR_LEN, src_mac);
    if (tagged) {
        wr_be16(hdrp + 12, ETHERTYPE_VLAN1Q);
        wr_be16(hdrp + 14, itf->vlan_id);
        wr_be16(hdrp + 16, ethertype);
    } else wr_be16(hdrp + 12, ethertype);

    bool ok = (net_tx_packet_on(ifindex, pkt) == 0);
    if (!ok) netpkt_unref(pkt);
    return ok;
}

void eth_input(uint8_t ifindex, netpkt_t* pkt) {
    l2_interface_t* input = l2_interface_find_by_index(ifindex);
    if (!pkt || !input || !input->is_up || netpkt_len(pkt) < sizeof(eth_hdr_t)) return;

    eth_hdr_t eth;
    if (!netpkt_copyout(pkt, 0, &eth, sizeof(eth))) return;

    uint16_t type = bswap16(eth.ethertype);
    l2_interface_t *logical = input;
    uint32_t header_size = sizeof(eth_hdr_t);

    if (type == ETHERTYPE_VLAN1Q) {
        if (input->dev_kind != NET_DEV_ETH || input->link_kind != NET_LINK_DIRECT) return;
        uint16_t vid = 0;
        if (!eth_parse_vlan(pkt, &vid, &type) || vid == 4095) return;
        logical = vid ? l2_vlan_find(ifindex, vid) : input;
        if (!logical || !logical->is_up) return;
        header_size += ETH_VLAN_TAG_LEN;
    }

    if (!netpkt_pull(pkt, header_size)) return;
    switch (type) {
        case ETHERTYPE_ARP:
            arp_input(logical->ifindex, eth.src_mac, pkt);
            break;
        case ETHERTYPE_IPV4:
            ipv4_input(logical->ifindex, pkt, eth.src_mac);
            break;
        case ETHERTYPE_IPV6:
            ipv6_input(logical->ifindex, pkt, eth.src_mac);
            break;
        case ETHERTYPE_VLAN1AD:
            break;
        default:
            break;
    }
}
