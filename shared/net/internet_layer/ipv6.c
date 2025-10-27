#include "ipv6.h"
#include "ipv6_utils.h"
#include "std/memory.h"
#include "std/string.h"
#include "net/link_layer/eth.h"
#include "console/kio.h"
#include "syscalls/syscalls.h"

void ipv6_send_packet(const uint8_t dst[16], uint8_t next_header, sizedptr segment, const ipv6_tx_opts_t* opts, uint8_t hop_limit) {
    if (!dst || !segment.ptr || !segment.size) return;
    if (!opts) return;
    if (opts->scope != IP_TX_BOUND_L2) return;

    uint8_t ifx = opts->index;
    uint8_t src[16];
    memset(src, 0, 16);

    uint8_t dst_mac[6];
    if (ipv6_is_multicast(dst)) ipv6_multicast_mac(dst, dst_mac);
    else return;

    uint32_t hdr_len = sizeof(ipv6_hdr_t);
    uint32_t total = hdr_len + (uint32_t)segment.size;
    uintptr_t buf = (uintptr_t)malloc(total);
    if (!buf) return;

    ipv6_hdr_t* ip6 = (ipv6_hdr_t*)buf;
    ip6->ver_tc_fl = bswap32((uint32_t)(6u << 28));
    ip6->payload_len = bswap16((uint16_t)segment.size);
    ip6->next_header = next_header;
    ip6->hop_limit = hop_limit ? hop_limit : 64;
    memcpy(ip6->src, src, 16);
    memcpy(ip6->dst, dst, 16);

    memcpy((void*)(buf + hdr_len), (const void*)segment.ptr, segment.size);

    sizedptr payload = { buf, total };
    eth_send_frame_on(ifx, ETHERTYPE_IPV6, dst_mac, payload);
    free((void*)buf, total);
}

void ipv6_input(uint16_t ifindex, uintptr_t ip_ptr, uint32_t ip_len, const uint8_t src_mac[6]) {
    if (ip_len < sizeof(ipv6_hdr_t)) return;

    ipv6_hdr_t* ip6 = (ipv6_hdr_t*)ip_ptr;
    uint32_t v = bswap32(ip6->ver_tc_fl);
    if ((v >> 28) != 6) return;

    uint16_t payload_len = bswap16(ip6->payload_len);
    if ((uint32_t)payload_len + sizeof(ipv6_hdr_t) > ip_len) return;

    uintptr_t l4 = ip_ptr + sizeof(ipv6_hdr_t);
    uint32_t l4_len = (uint32_t)payload_len;
    (void)ifindex;
    (void)src_mac;
    (void)l4;
    (void)l4_len;
    kprintf("ipv6");
}