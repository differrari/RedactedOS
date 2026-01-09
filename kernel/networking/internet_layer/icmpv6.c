#include "icmpv6.h"
#include "std/memory.h"
#include "net/checksums.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/internet_layer/ipv6_route.h"
#include "networking/link_layer/eth.h"
#include "networking/link_layer/ndp.h"
#include "networking/internet_layer/mld.h"
#include "syscalls/syscalls.h"

#define MAX_PENDING 16

typedef struct __attribute__((packed)) {
    icmpv6_hdr_t hdr;
    uint16_t id;
    uint16_t seq;
} icmpv6_echo_t;

typedef struct {
    bool in_use;
    uint16_t id;
    uint16_t seq;
    bool received;
    uint8_t rx_type;
    uint8_t rx_code;
    uint32_t start_ms;
    uint32_t end_ms;
    uint8_t rx_src_ip[16];
} ping6_slot_t;

static ping6_slot_t g_pending[MAX_PENDING] = {0};

static int alloc_slot(uint16_t id, uint16_t seq) {
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!g_pending[i].in_use) {
            g_pending[i].in_use = true;
            g_pending[i].id = id;
            g_pending[i].seq = seq;
            g_pending[i].received = false;
            g_pending[i].rx_type = 0xFF;
            g_pending[i].rx_code = 0xFF;
            g_pending[i].start_ms = (uint32_t)get_time();
            g_pending[i].end_ms = 0;
            memset(g_pending[i].rx_src_ip, 0, 16);
            return i;
        }
    }
    return -1;
}

static void mark_received(uint16_t id, uint16_t seq, uint8_t type, uint8_t code, const uint8_t src_ip[16]) {
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g_pending[i].in_use && g_pending[i].id == id && g_pending[i].seq == seq) {
            g_pending[i].received = true;
            g_pending[i].rx_type = type;
            g_pending[i].rx_code = code;
            g_pending[i].end_ms = (uint32_t)get_time();
            if (src_ip) memcpy(g_pending[i].rx_src_ip, src_ip, 16);
            return;
        }
    }
}

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

    ipv6_hdr_t *ip6 = (ipv6_hdr_t*)buf;
    ip6->ver_tc_fl = bswap32((uint32_t)(6u << 28));
    ip6->payload_len = bswap16((uint16_t)icmp_len);
    ip6->next_header = 58;
    ip6->hop_limit = hop_limit ? hop_limit : 64;
    memcpy(ip6->src, src_ip, 16);
    memcpy(ip6->dst, dst_ip, 16);

    memcpy((void*)((uintptr_t)buf + sizeof(ipv6_hdr_t)), icmp, icmp_len);

    return eth_send_frame_on(ifindex, ETHERTYPE_IPV6, dst_mac, pkt);
}

static bool icmpv6_send_echo_reply(uint16_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], const uint8_t *icmp, uint32_t icmp_len, const uint8_t src_mac[6], uint8_t hop_limit) {
    if (!dst_ip || !icmp || icmp_len < sizeof(icmpv6_echo_t)) return false;

    uintptr_t buf = (uintptr_t)malloc(icmp_len);
    if (!buf) return false;

    memcpy((void*)buf, icmp, icmp_len);

    icmpv6_echo_t *e = (icmpv6_echo_t*)buf;
    e->hdr.type = ICMPV6_ECHO_REPLY;
    e->hdr.code = 0;
    e->hdr.checksum = 0;

    ipv6_tx_plan_t plan;
    if (!ipv6_build_tx_plan(dst_ip, 0 ,0, 0, &plan)) {
        free_sized((void*)buf, icmp_len);
        return false;
    }

    e->hdr.checksum = bswap16(checksum16_pipv6(dst_ip, src_ip, 58, (const uint8_t*)buf, icmp_len));

    icmpv6_send_on_l2(ifindex, src_ip, dst_ip, src_mac, (const void*)buf, icmp_len, hop_limit ? hop_limit : 64);

    free_sized((void*)buf, icmp_len);
    return true;
}

static bool icmpv6_send_echo_request(const uint8_t dst_ip[16], uint16_t id, uint16_t seq, const void *payload, uint32_t payload_len, const void *tx_opts_or_null, uint8_t hop_limit) {
    if (!dst_ip) return false;

    uint32_t len = (uint32_t)sizeof(icmpv6_echo_t) + payload_len;
    uint32_t headroom = (uint32_t)sizeof(eth_hdr_t) + (uint32_t)sizeof(ipv6_hdr_t);
    netpkt_t* pkt = netpkt_alloc(len, headroom, 0);
    if (!pkt) return false;
    void* buf = netpkt_put(pkt, len);
    if (!buf) {
        netpkt_unref(pkt);
        return false;
    }

    icmpv6_echo_t *e = (icmpv6_echo_t*)buf;
    e->hdr.type = ICMPV6_ECHO_REQUEST;
    e->hdr.code = 0;
    e->hdr.checksum = 0;
    e->id = bswap16(id);
    e->seq = bswap16(seq);

    if (payload_len) memcpy((void*)((uintptr_t)buf + sizeof(icmpv6_echo_t)), payload, payload_len);

    ipv6_tx_plan_t plan;
    if (!ipv6_build_tx_plan(dst_ip, tx_opts_or_null, 0, 0, &plan)) {
        netpkt_unref(pkt);
        return false;
    }
    e->hdr.checksum = bswap16(checksum16_pipv6(plan.src_ip, dst_ip, 58, (const uint8_t*)buf, len));

    ipv6_send_packet(dst_ip, 58, pkt, (const ipv6_tx_opts_t*)tx_opts_or_null, hop_limit ? hop_limit : 64, 0);
    return true;
}

bool icmpv6_ping(const uint8_t dst_ip[16], uint16_t id, uint16_t seq, uint32_t timeout_ms, const void *tx_opts_or_null, uint8_t hop_limit, ping6_result_t *out) {
    int slot = alloc_slot(id, seq);
    if (slot < 0) {
        if (out) {
            out->rtt_ms = 0;
            out->status = PING_UNKNOWN_ERROR;
            out->icmp_type = 0xFF;
            out->icmp_code = 0xFF;
            memset(out->responder_ip, 0, 16);
        }
        return false;
    }

    uint8_t payload[32];
    memset(payload, 0, sizeof(payload));

    if (!icmpv6_send_echo_request(dst_ip, id, seq, payload, sizeof(payload), tx_opts_or_null, hop_limit)) {
        if (out) {
            out->rtt_ms = 0;
            out->status = PING_UNKNOWN_ERROR;
            out->icmp_type = 0xFF;
            out->icmp_code = 0xFF;
            memset(out->responder_ip, 0, 16);
        }
        g_pending[slot].in_use =false;
        return false;
    }

    uint32_t start = (uint32_t)get_time();
    for (;;) {
        if (g_pending[slot].received) {
            if (out) {
                out->icmp_type = g_pending[slot].rx_type;
                out->icmp_code = g_pending[slot].rx_code;
                memcpy(out->responder_ip, g_pending[slot].rx_src_ip, 16);

                switch (g_pending[slot].rx_type) {
                case ICMPV6_ECHO_REPLY:
                    out->status = PING_OK;
                    break;
                case ICMPV6_DEST_UNREACH:
                    switch (g_pending[slot].rx_code) {
                        case 0: out->status = PING_NET_UNREACH; break;
                        case 1: out->status = PING_ADMIN_PROHIBITED; break;
                        case 2: out->status = PING_ADMIN_PROHIBITED; break;
                        case 3: out->status = PING_HOST_UNREACH; break;
                        case 4: out->status = PING_PORT_UNREACH; break;
                        default: out->status = PING_UNKNOWN_ERROR; break;
                    }
                    break;
                case ICMPV6_PACKET_TOO_BIG:
                    out->status = PING_FRAG_NEEDED;
                    break;
                case ICMPV6_TIME_EXCEEDED:
                    out->status = PING_TTL_EXPIRED;
                    break;
                case ICMPV6_PARAM_PROBLEM:
                    out->status = PING_PARAM_PROBLEM;
                    break;
                default:
                    out->status = PING_UNKNOWN_ERROR;
                    break;
                }

                if (g_pending[slot].end_ms >= g_pending[slot].start_ms) out->rtt_ms = g_pending[slot].end_ms - g_pending[slot].start_ms;
                else out->rtt_ms = 0;
            }

            bool ok = (g_pending[slot].rx_type == ICMPV6_ECHO_REPLY);
            g_pending[slot].in_use = false;
            return ok;
        }

        uint32_t now = (uint32_t)get_time();
        if (now - start >= timeout_ms) break;
        msleep(5);
    }

    if (out) {
        out->rtt_ms = 0;
        out->status = PING_TIMEOUT;
        out->icmp_type = 0xFF;
        out->icmp_code = 0xFF;
        memset(out->responder_ip, 0, 16);
    }

    g_pending[slot].in_use = false;
    return false;
}

static bool extract_echo_id_seq_from_error(const uint8_t *icmp, uint32_t icmp_len, uint16_t *out_id, uint16_t *out_seq) {//b
    if (!icmp || icmp_len < 8u + (uint32_t)sizeof(ipv6_hdr_t) + (uint32_t)sizeof(icmpv6_echo_t)) return false;

    const ipv6_hdr_t *inner = (const ipv6_hdr_t*)(icmp + 8);
    uint32_t v = bswap32(inner->ver_tc_fl);
    if ((v >>28) != 6) return false;
    if (inner->next_header != 58) return false;

    const uint8_t *inner_icmp = (const uint8_t*)(inner + 1);
    if ((uintptr_t)inner_icmp + sizeof(icmpv6_echo_t)>(uintptr_t)icmp + icmp_len) return false;

    const icmpv6_echo_t *e = (const icmpv6_echo_t*)inner_icmp;
    if (e->hdr.type != ICMPV6_ECHO_REQUEST) return false;

    if (out_id) *out_id = bswap16(e->id);
    if (out_seq) *out_seq = bswap16(e->seq);
    return true;
}

void icmpv6_input(uint16_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], uint8_t hop_limit, const uint8_t src_mac[6], const uint8_t *icmp, uint32_t icmp_len) {
    if (!ifindex || !src_ip || !dst_ip || !icmp || icmp_len < sizeof(icmpv6_hdr_t)) return;

    const icmpv6_hdr_t *h = (const icmpv6_hdr_t*)icmp;
    if (h->code != 0 && (h->type == ICMPV6_ECHO_REQUEST || h->type == ICMPV6_ECHO_REPLY)) return;

    uint16_t calc = bswap16(checksum16_pipv6(src_ip, dst_ip, 58, icmp, icmp_len));
    if (calc != 0) return;

    if ((h->type == 133 || h->type == 134 || h->type == 135 || h->type == 136 || h->type == 137) && hop_limit != 255) return;
    if (h->type == 130 || h->type == 131 || h->type == 132 || h->type == 143) {
        mld_input((uint8_t)ifindex, src_ip, dst_ip, icmp, icmp_len);
        return;
    }


    if (h->type == ICMPV6_ECHO_REQUEST) {
        icmpv6_send_echo_reply(ifindex, src_ip, dst_ip, icmp, icmp_len, src_mac, hop_limit);
        return;
    }

    if (h->type == ICMPV6_ECHO_REPLY) {
        if (icmp_len < sizeof(icmpv6_echo_t)) return;
        const icmpv6_echo_t *e = (const icmpv6_echo_t*)icmp;
        mark_received(bswap16(e->id), bswap16(e->seq), h->type, h->code, src_ip);
        return;
    }

    if (h->type == 133 || h->type == 134 || h->type == 135 || h->type == 136 || h->type == 137) {
        ndp_input(ifindex, src_ip, dst_ip, src_mac, icmp, icmp_len);
        return;
    }

    if (h->type == ICMPV6_PACKET_TOO_BIG) {

        if (icmp_len >= 8u + (uint32_t)sizeof(ipv6_hdr_t)) {
            uint32_t mtu = bswap32(*(const uint32_t *)(icmp + 4));
            const ipv6_hdr_t *inner = (const ipv6_hdr_t *)(icmp + 8);
            uint32_t v = bswap32(inner->ver_tc_fl);

            if ((v >> 28) == 6 && mtu >= 1280u && mtu <= 65535u)
                ipv6_pmtu_note(inner->dst, (uint16_t)mtu);

            uint16_t id = 0, seq = 0;
            if (extract_echo_id_seq_from_error(icmp, icmp_len, &id, &seq))
                mark_received(id, seq, h->type, h->code, src_ip);
        }
        return;
    }

    if (h->type == ICMPV6_DEST_UNREACH || h->type == ICMPV6_TIME_EXCEEDED || h->type == ICMPV6_PARAM_PROBLEM) {
        uint16_t id = 0, seq = 0;
        if (extract_echo_id_seq_from_error(icmp, icmp_len, &id, &seq)) mark_received(id, seq, h->type, h->code, src_ip);
        return;
    }
}