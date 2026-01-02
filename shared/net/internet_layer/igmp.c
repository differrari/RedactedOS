#include "igmp.h"
#include "net/internet_layer/ipv4.h"
#include "net/internet_layer/ipv4_utils.h"
#include "net/checksums.h"
#include "networking/interface_manager.h"
#include "std/memory.h"
#include "std/string.h"

#define IGMP_TYPE_QUERY 0x11
#define IGMP_TYPE_V2_REPORT 0x16
#define IGMP_TYPE_V2_LEAVE 0x17

typedef struct __attribute__((packed)) igmp_hdr_t {
    uint8_t type;
    uint8_t max_resp_time;
    uint16_t checksum;
    uint32_t group;
} igmp_hdr_t;

static bool send_igmp(uint8_t ifindex, uint32_t dst, uint8_t type, uint32_t group) {
    uint32_t headroom = (uint32_t)sizeof(eth_hdr_t) + (uint32_t)sizeof(ipv4_hdr_t);
    netpkt_t* pkt = netpkt_alloc(sizeof(igmp_hdr_t),headroom, 0);
    if (!pkt) return false;

    igmp_hdr_t* h = (igmp_hdr_t*)netpkt_put(pkt, sizeof(igmp_hdr_t));
    if (!h) {
        netpkt_unref(pkt);
        return false;
    }

    h->type = type;
    h->max_resp_time = 0;
    h->group = bswap32(group);
    h->checksum = 0;
    h->checksum = checksum16((const uint16_t*)h, sizeof(igmp_hdr_t)/2);

    ipv4_tx_opts_t tx;
    tx.scope = IP_TX_BOUND_L2;
    tx.index = ifindex;

    ipv4_send_packet(dst, 2, pkt, &tx, 1, 0);
    return true;
}

bool igmp_send_join(uint8_t ifindex, uint32_t group) {
    if (!ipv4_is_multicast(group)) return false;
    return send_igmp(ifindex, group, IGMP_TYPE_V2_REPORT, group);
}

bool igmp_send_leave(uint8_t ifindex, uint32_t group) {
    if (!ipv4_is_multicast(group)) return false;
    return send_igmp(ifindex, 0xE0000002u, IGMP_TYPE_V2_LEAVE, group);
}

static void send_reports_for_interface(uint8_t ifindex) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return;
    for (int i = 0; i < (int)l2->ipv4_mcast_count; ++i) {
        uint32_t g = l2->ipv4_mcast[i];
        if (ipv4_is_multicast(g)) (void)send_igmp(ifindex,g, IGMP_TYPE_V2_REPORT, g);
    }
}

void igmp_input(uint8_t ifindex, uint32_t src, uint32_t dst, const void* l4, uint32_t l4_len) {
    if (!l4 || l4_len < sizeof(igmp_hdr_t)) return;
    const igmp_hdr_t* h = (const igmp_hdr_t*)l4;
    uint16_t saved = h->checksum;
    igmp_hdr_t tmp;
    memcpy(&tmp, h, sizeof(tmp));
    tmp.checksum = 0;
    if (checksum16((const uint16_t*)&tmp, sizeof(tmp) / 2) != saved) return;

    uint8_t type = h->type;
    uint32_t group = bswap32(h->group);

    if (type != IGMP_TYPE_QUERY) return;

    if (group == 0) {
        send_reports_for_interface(ifindex);
        return;
    }

    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return;

    for (int i = 0; i < (int)l2->ipv4_mcast_count; ++i) {
        if (l2->ipv4_mcast[i] == group) {
            (void)send_igmp(ifindex, group, IGMP_TYPE_V2_REPORT, group);
            return;
        }
    }

    (void)src;
    (void)dst;
}