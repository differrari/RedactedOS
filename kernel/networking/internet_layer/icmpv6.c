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

static bool icmpv6_send_echo_reply(uint16_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], const uint8_t *icmp, uint32_t icmp_len, const uint8_t src_mac[6], uint8_t hop_limit) {
    if (!dst_ip || !icmp || icmp_len < sizeof(icmpv6_echo_t)) return false;

    uintptr_t buf = (uintptr_t)zalloc(icmp_len ? icmp_len : 1u);
    if (!buf) return false;

    memcpy((void*)buf, icmp, icmp_len);

    icmpv6_echo_t *e = (icmpv6_echo_t*)buf;
    e->hdr.type = ICMPV6_ECHO_REPLY;
    e->hdr.code = 0;
    e->hdr.checksum = 0;

    ipv6_tx_plan_t plan;
    if (!ipv6_build_tx_plan(dst_ip, 0, &plan)) {
        release((void*)buf);
        return false;
    }

    e->hdr.checksum = bswap16(checksum16_pipv6(dst_ip, src_ip, PROTO_ICMPV6, (const uint8_t*)buf, icmp_len));

    icmpv6_send_on_l2(ifindex, src_ip, dst_ip, src_mac, (const void*)buf, icmp_len, hop_limit ? hop_limit : 64);

    release((void*)buf);
    return true;
}

void icmpv6_input(uint16_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], uint8_t hop_limit, const uint8_t src_mac[6], netpkt_t* pkt) {
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

    if ((h->type == 133 || h->type == 134 || h->type == 135 || h->type == 136 || h->type == 137) && hop_limit != 255) {
        netpkt_unref(pkt);
        return;
    }

    socket_raw_input_v6((uint8_t)ifindex, src_ip, dst_ip, pkt);
    if (h->type == 130 || h->type == 131 || h->type == 132 || h->type == 143) {
        mld_input((uint8_t)ifindex, src_ip, dst_ip, pkt);
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

    if (h->type == 133 || h->type == 134 || h->type == 135 || h->type == 136 || h->type == 137) {
        ndp_input(ifindex, src_ip, dst_ip, src_mac, pkt);
        netpkt_unref(pkt);
        return;
    }

    if (h->type == ICMPV6_PACKET_TOO_BIG) {

        if (icmp_len >= 8u + (uint32_t)sizeof(ipv6_hdr_t)) {
            uint32_t mtu = rd_be32(icmp + 4);
            ipv6_hdr_t inner;
            memcpy(&inner, icmp + 8, sizeof(inner));
            uint32_t v = bswap32(inner.ver_tc_fl);

            if ((v >> 28) == 6 && mtu >= 1280u && mtu <= 65535u) ipv6_pmtu_note(inner.dst, (uint16_t)mtu);
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