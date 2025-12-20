#include "icmpv6.h"
#include "std/memory.h"
#include "net/checksums.h"
#include "net/internet_layer/ipv6.h"
#include "net/internet_layer/ipv6_utils.h"
#include "net/link_layer/eth.h"
#include "net/link_layer/ndp.h"
#include "syscalls/syscalls.h"

typedef struct __attribute__((packed)) {
    icmpv6_hdr_t hdr;
    uint16_t id;
    uint16_t seq;
} icmpv6_echo_t;

bool icmpv6_send_on_l2(uint8_t ifindex, const uint8_t dst_ip[16], const uint8_t src_ip[16], const uint8_t dst_mac[6], const void *icmp, uint32_t icmp_len, uint8_t hop_limit) {
    if (!ifindex || !dst_ip || !src_ip || !dst_mac || !icmp || !icmp_len) return false;

    uint32_t total = (uint32_t)sizeof(ipv6_hdr_t) + icmp_len;
    uintptr_t buf =(uintptr_t)malloc(total);
    if (!buf) return false;

    ipv6_hdr_t *ip6 = (ipv6_hdr_t*)buf;
    ip6->ver_tc_fl = bswap32((uint32_t)(6u << 28));
    ip6->payload_len = bswap16((uint16_t)icmp_len);
    ip6->next_header = 58;
    ip6->hop_limit = hop_limit;
    memcpy(ip6->src, src_ip, 16);
    memcpy(ip6->dst, dst_ip, 16);

    memcpy((void*)(buf + sizeof(ipv6_hdr_t)), icmp, icmp_len);

    sizedptr payload = (sizedptr){ buf, total };
    eth_send_frame_on(ifindex, ETHERTYPE_IPV6, dst_mac, payload);

    free((void*)buf, total);
    return true;
}

static bool icmpv6_send_echo_reply(uint16_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], const uint8_t *icmp, uint32_t icmp_len, const uint8_t src_mac[6]) {
    if (!ifindex || !src_ip || !dst_ip || !icmp || icmp_len < sizeof(icmpv6_echo_t) || !src_mac) return false;

    uintptr_t buf = (uintptr_t)malloc(icmp_len);
    if (!buf) return false;

    memcpy((void*)buf, icmp, icmp_len);

    icmpv6_echo_t *e = (icmpv6_echo_t*)buf;
    e->hdr.type = 129;
    e->hdr.code = 0;
    e->hdr.checksum = 0;
    e->hdr.checksum = bswap16(checksum16_pipv6(dst_ip, src_ip, 58, (const uint8_t*)buf, icmp_len));

    ip_tx_opts_t opts;
    opts.index = (uint8_t)ifindex;
    opts.scope = IP_TX_BOUND_L2;

    ipv6_send_packet(src_ip, 58, (sizedptr){ buf, icmp_len }, (const void*)&opts, 64);

    free((void*)buf, icmp_len);
    return true;
}

bool icmpv6_send_echo_request(const uint8_t dst_ip[16], uint16_t id, uint16_t seq, const void *payload, uint32_t payload_len, const void *tx_opts_or_null) {
    if (!dst_ip) return false;

    uint32_t len = (uint32_t)sizeof(icmpv6_echo_t) + payload_len;
    uintptr_t buf = (uintptr_t)malloc(len);
    if (!buf) return false;

    icmpv6_echo_t *e = (icmpv6_echo_t*)buf;
    e->hdr.type = 128;
    e->hdr.code = 0;
    e->hdr.checksum = 0;
    e->id = bswap16(id);
    e->seq = bswap16(seq);

    if (payload_len) memcpy((void*)(buf + sizeof(icmpv6_echo_t)), payload, payload_len);

    uint8_t src_ip[16] = {0};
    e->hdr.checksum = bswap16(checksum16_pipv6(src_ip, dst_ip, 58, (const uint8_t*)buf, len));

    ipv6_send_packet(dst_ip, 58, (sizedptr){ buf, len }, tx_opts_or_null, 64);

    free((void*)buf, len);
    return true;
}

void icmpv6_input(uint16_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], uint8_t hop_limit, const uint8_t src_mac[6], const uint8_t *icmp, uint32_t icmp_len) {
    if (!ifindex || !src_ip || !dst_ip || !icmp || icmp_len < sizeof(icmpv6_hdr_t)) return;

    const icmpv6_hdr_t *h = (const icmpv6_hdr_t*)icmp;
    if (h->code != 0 && (h->type == 128 || h->type == 129)) return;

    uint16_t calc = bswap16(checksum16_pipv6(src_ip, dst_ip, 58, icmp, icmp_len));
    if (calc != 0) return;

    if ((h->type == 133 || h->type == 134 || h->type == 135 || h->type == 136 || h->type == 137) && hop_limit != 255) return;

    if (h->type == 128) {
        icmpv6_send_echo_reply(ifindex, src_ip, dst_ip, icmp, icmp_len, src_mac);
        return;
    }

    if (h->type == 133 || h->type == 134 || h->type == 135 || h->type == 136 || h->type == 137) {
        ndp_input(ifindex, src_ip, dst_ip, src_mac, icmp, icmp_len);
        return;
    }
}