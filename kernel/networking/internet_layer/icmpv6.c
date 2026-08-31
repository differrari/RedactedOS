#include "icmpv6.h"
#include "std/memory.h"
#include "net/checksums.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/internet_layer/ipv6_route.h"
#include "networking/link_layer/eth.h"
#include "networking/link_layer/ndp.h"
#include "networking/internet_layer/mld.h"
#include "networking/transport_layer/csocket_raw.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/transport_layer/tcp.h"
#include "networking/internet_layer/pmtu.h"
#include "networking/interface_manager.h"

typedef struct __attribute__((packed)) {
    icmpv6_hdr_t hdr;
    uint16_t id;
    uint16_t seq;
} icmpv6_echo_t;

bool icmpv6_send_on_l2(uint8_t ifindex, const uint8_t dst_ip[16], const uint8_t src_ip[16], const uint8_t dst_mac[6], const void *icmp, uint32_t icmp_len, uint8_t hop_limit) {
    if (!ifindex || !dst_ip || !src_ip || !dst_mac || !icmp || !icmp_len) return false;

    uint32_t total = (uint32_t)sizeof(ipv6_hdr_t) + icmp_len;
    netpkt_t* pkt = netpkt_alloc(total, (uint32_t)sizeof(eth_hdr_t), 0);
    if (!pkt) return false;
    void* buf = netpkt_put(pkt, total);
    if (!buf) {
        netpkt_unref(pkt);
        return false;
    }

    ipv6_hdr_t ip6;
    ip6.ver_tc_fl = bswap32((uint32_t)(6u << 28));
    ip6.payload_len = bswap16((uint16_t)icmp_len);
    ip6.next_header = PROTO_ICMPV6;
    ip6.hop_limit = hop_limit ? hop_limit : 64;
    ipv6_cpy(ip6.src, src_ip);
    ipv6_cpy(ip6.dst, dst_ip);

    memcpy(buf, &ip6, sizeof(ip6));
    memcpy((void*)((uintptr_t)buf + sizeof(ipv6_hdr_t)), icmp, icmp_len);

    return eth_send_frame_on(ifindex, ETHERTYPE_IPV6, dst_mac, pkt);
}

static bool icmpv6_send_echo_reply(uint8_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], const uint8_t *icmp, uint32_t icmp_len, const uint8_t src_mac[6], uint8_t hop_limit) {
    if (!dst_ip || !icmp || icmp_len < sizeof(icmpv6_echo_t)) return false;

    uintptr_t buf = (uintptr_t)zalloc(icmp_len ? icmp_len : 1u);
    if (!buf) return false;

    memcpy((void*)buf, icmp, icmp_len);

    icmpv6_echo_t *e = (icmpv6_echo_t*)buf;
    e->hdr.type = ICMPV6_ECHO_REPLY;
    e->hdr.code = 0;
    e->hdr.checksum = 0;

    e->hdr.checksum = bswap16(checksum16_pipv6(dst_ip, src_ip, PROTO_ICMPV6, (const uint8_t*)buf, icmp_len));

    icmpv6_send_on_l2(ifindex, src_ip, dst_ip, src_mac, (const void*)buf, icmp_len, hop_limit ? hop_limit : 64);

    release((void*)buf);
    return true;
}

void icmpv6_input(uint8_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], uint8_t hop_limit, const uint8_t src_mac[6], netpkt_t* pkt) {
    if (!ifindex || !src_ip || !dst_ip || !pkt || netpkt_len(pkt) < sizeof(icmpv6_hdr_t)) {
        if (pkt) netpkt_unref(pkt);
        return;
    }

    const uint8_t *icmp = (const uint8_t*)netpkt_data(pkt);
    uint32_t icmp_len = netpkt_len(pkt);
    icmpv6_hdr_t hdr;
    if (!netpkt_copyout(pkt, 0, &hdr, sizeof(hdr))) {
        netpkt_unref(pkt);
        return;
    }
    const icmpv6_hdr_t *h = &hdr;
    if (h->code != 0 && (h->type == ICMPV6_ECHO_REQUEST || h->type == ICMPV6_ECHO_REPLY)) {
        netpkt_unref(pkt);
        return;
    }

    uint16_t calc = bswap16(checksum16_pipv6(src_ip, dst_ip, PROTO_ICMPV6, icmp, icmp_len));
    if (calc != 0) {
        netpkt_unref(pkt);
        return;
    }

    if ((h->type == ICMPV6_ROUTER_SOLICIT || h->type == ICMPV6_ROUTER_ADVERT || h->type == ICMPV6_NEIGHBOR_SOLICIT || h->type == ICMPV6_NEIGHBOR_ADVERT || h->type == ICMPV6_REDIRECT) && hop_limit != 255) {
        netpkt_unref(pkt);
        return;
    }

    socket_raw_input_v6(ifindex, src_ip, dst_ip, pkt);
    if (h->type == ICMPV6_MLD_QUERY || h->type == ICMPV6_MLD_REPORT || h->type == ICMPV6_MLD_DONE || h->type == ICMPV6_MLDV2_REPORT) {
        mld_input(ifindex, src_ip, dst_ip, pkt);
        netpkt_unref(pkt);
        return;
    }

    if (h->type == ICMPV6_ECHO_REQUEST) {
        icmpv6_send_echo_reply(ifindex, src_ip, dst_ip, icmp, icmp_len, src_mac, hop_limit);
        netpkt_unref(pkt);
        return;
    }

    if (h->type == ICMPV6_ECHO_REPLY) {
        netpkt_unref(pkt);
        return;
    }

    if (h->type == ICMPV6_ROUTER_SOLICIT || h->type == ICMPV6_ROUTER_ADVERT || h->type == ICMPV6_NEIGHBOR_SOLICIT || h->type == ICMPV6_NEIGHBOR_ADVERT || h->type == ICMPV6_REDIRECT) {
        ndp_input(ifindex, src_ip, dst_ip, src_mac, pkt);
        netpkt_unref(pkt);
        return;
    }

    if (h->type == ICMPV6_PACKET_TOO_BIG) {
        if (h->code || icmp_len < 8 + sizeof(ipv6_hdr_t)) {
            netpkt_unref(pkt);
            return;
        }

        uint32_t reported = rd_be32(icmp + 4);
        if (reported < 1280 || reported > UINT16_MAX) {
            netpkt_unref(pkt);
            return;
        }

        ipv6_hdr_t inner;
        memcpy(&inner, icmp + 8, sizeof(inner));
        if ((bswap32(inner.ver_tc_fl) >> 28) != 6 || ipv6_is_multicast(inner.dst)) {
            netpkt_unref(pkt);
            return;
        }

        l3_ipv6_interface_t* l3 = l3_ipv6_find_by_ip(inner.src);
        if (!ipv6_l3_is_ready(l3) || !l3->l2 || l3->l2->ifindex != ifindex) {
            netpkt_unref(pkt);
            return;
        }

        uint16_t mtu = (uint16_t)reported;
        uint16_t base_mtu = l3_ipv6_effective_mtu(l3);
        if (base_mtu && mtu < base_mtu) {
            uint16_t path_mtu = pmtu_note(l3->l3_id, l3->epoch, IP_VER6, inner.dst, mtu);
            if (path_mtu && inner.next_header == PROTO_TCP && icmp_len >= 8 + sizeof(ipv6_hdr_t) + 4) {
                const uint8_t* tcp = icmp + 8 + sizeof(ipv6_hdr_t);
                tcp_pmtu_update(l3->l3_id, IP_VER6, inner.src, inner.dst, rd_be16(tcp), rd_be16(tcp + 2), path_mtu);
            }
        }

        netpkt_unref(pkt);
        return;
    }

    if (h->type == ICMPV6_DEST_UNREACH || h->type == ICMPV6_TIME_EXCEEDED || h->type == ICMPV6_PARAM_PROBLEM) {
        netpkt_unref(pkt);
        return;
    }

    netpkt_unref(pkt);
}