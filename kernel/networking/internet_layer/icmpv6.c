#include "icmpv6.h"
#include "std/memory.h"
#include "net/checksums.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/internet_layer/ipv6_route.h"
#include "networking/link_layer/eth.h"
#include "networking/link_layer/ndp.h"
#include "networking/internet_layer/mld.h"
#include "networking/transport_layer/csocket.h"
#include "networking/transport_layer/csocket_raw.h"
#include "networking/transport_layer/trans_utils.h"
#include "syscalls/syscalls.h"

typedef struct __attribute__((packed)) {
    icmpv6_hdr_t hdr;
    uint16_t id;
    uint16_t seq;
} icmpv6_echo_t;

static bool extract_echo_id_seq_from_error(const uint8_t *icmp, uint32_t icmp_len, uint16_t *out_id, uint16_t *out_seq);

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

uint32_t icmpv6_ping_collect(const uint8_t dst_ip[16], uint16_t id, uint16_t seq, uint32_t timeout_ms, const void *tx_opts_or_null, uint8_t hop_limit, ping6_result_t *out, uint32_t max_results) {
    if (!out || !max_results) return 0;

    SocketOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.special_kind = SOCKET_SPECIAL_RAW;
    if (hop_limit) {
        opt.flags |= SOCK_OPT_TTL;
        opt.ttl = hop_limit;
    }
    socket_handle_t sock = create_socket(PROTO_ICMPV6, &opt);
    if (!sock) {
        return 0;
    }

    const ip_tx_opts_t* tx = (const ip_tx_opts_t*)tx_opts_or_null;
    if (tx && tx->scope != IP_TX_AUTO) {
        SockBindSpec bind;
        memset(&bind, 0, sizeof(bind));
        if (tx->scope == IP_TX_BOUND_L2) {
            bind.kind = BIND_L2;
            bind.ifindex = tx->index;
        } else if (tx->scope == IP_TX_BOUND_L3) {
            bind.kind = BIND_L3;
            bind.ver = IP_VER6;
            bind.l3_id = tx->index;
        }
        if (bind.kind && bind_socket(sock, &bind, 0) != SOCK_OK) {
            close_socket(sock);
            return 0;
        }
    }

    uint8_t payload[sizeof(icmpv6_echo_t) + 32];
    memset(payload, 0, sizeof(payload));
    icmpv6_echo_t* e = (icmpv6_echo_t*)payload;
    e->hdr.type = ICMPV6_ECHO_REQUEST;
    e->hdr.code = 0;
    e->id = bswap16(id);
    e->seq = bswap16(seq);

    net_l4_endpoint dst;
    memset(&dst, 0, sizeof(dst));
    dst.ver = IP_VER6;
    ipv6_cpy(dst.ip, dst_ip);
    if (send_to_socket(sock, &dst, payload, sizeof(payload)) != (int64_t)sizeof(payload)) {
        close_socket(sock);
        return 0;
    }

    uint32_t count = 0;
    uint32_t start = (uint32_t)get_time();
    while (count < max_results) {
        uint32_t now = (uint32_t)get_time();
        if (now - start >= timeout_ms) break;

        uint8_t rx[1280];
        net_l4_endpoint src;
        memset(&src, 0, sizeof(src));
        int64_t n = receive_from_socket(sock, rx, sizeof(rx), &src);
        if (n == SOCK_ERR_WOULDBLOCK) {
            msleep(5);
            continue;
        }

        if (n < (int64_t)sizeof(icmpv6_hdr_t)) {
        if (n < 0) msleep(5);
        continue;
        }

        uint8_t type = rx[0];
        uint8_t code = rx[1];
        bool matched = false;
        if (type == ICMPV6_ECHO_REPLY && n >= (int64_t)sizeof(icmpv6_echo_t)) {
            icmpv6_echo_t reply;
            memcpy(&reply, rx, sizeof(reply));
            if (bswap16(reply.id) == id &&bswap16(reply.seq) == seq) matched = true;
        } else if (type == ICMPV6_DEST_UNREACH || type == ICMPV6_PACKET_TOO_BIG || type == ICMPV6_TIME_EXCEEDED || type == ICMPV6_PARAM_PROBLEM) {
            uint16_t inner_id = 0;
            uint16_t inner_seq = 0;
            if (extract_echo_id_seq_from_error(rx, (uint32_t)n, &inner_id, &inner_seq) && inner_id == id && inner_seq == seq) matched = true;
        }
        if (!matched) continue;

        ping6_result_t* r = &out[count++];
        memset(r, 0, sizeof(*r));
        r->icmp_type = type;
        r->icmp_code = code;
        ipv6_cpy(r->responder_ip, src.ip);
        now = (uint32_t)get_time();
        r->rtt_ms = now >= start ? now - start : 0;
        switch (type) {
            case ICMPV6_ECHO_REPLY:
                r->status = PING_OK;
                break;
            case ICMPV6_DEST_UNREACH:
                switch (code) {
                    case 0: r->status = PING_NET_UNREACH; break;
                    case 1: r->status = PING_ADMIN_PROHIBITED; break;
                    case 2: r->status = PING_ADMIN_PROHIBITED; break;
                    case 3: r->status = PING_HOST_UNREACH; break;
                    case 4: r->status = PING_PORT_UNREACH; break;
                    default: r->status = PING_UNKNOWN_ERROR; break;
                }
                break;
            case ICMPV6_PACKET_TOO_BIG: r->status = PING_FRAG_NEEDED; break;
            case ICMPV6_TIME_EXCEEDED: r->status = PING_TTL_EXPIRED; break;
            case ICMPV6_PARAM_PROBLEM: r->status = PING_PARAM_PROBLEM; break;
            default: r->status = PING_UNKNOWN_ERROR; break;
        }
        if (max_results == 1) break;
    }

    close_socket(sock);
    return count;
}

bool icmpv6_ping(const uint8_t dst_ip[16], uint16_t id, uint16_t seq, uint32_t timeout_ms, const void *tx_opts_or_null, uint8_t hop_limit, ping6_result_t *out) {
    ping6_result_t res;
    ping6_result_t *dst = out ? out : &res;
    uint32_t n = icmpv6_ping_collect(dst_ip, id, seq, timeout_ms, tx_opts_or_null, hop_limit, dst, 1);
    return n && dst->status == PING_OK;
}

static bool extract_echo_id_seq_from_error(const uint8_t *icmp, uint32_t icmp_len, uint16_t *out_id, uint16_t *out_seq) {
    if (!icmp || icmp_len < 8u + (uint32_t)sizeof(ipv6_hdr_t) + (uint32_t)sizeof(icmpv6_echo_t)) return false;

    ipv6_hdr_t inner;
    memcpy(&inner, icmp + 8, sizeof(inner));
    uint32_t v = bswap32(inner.ver_tc_fl);
    if ((v >>28) != 6) return false;
    if (inner.next_header != PROTO_ICMPV6) return false;

    const uint8_t *inner_icmp = icmp + 8u + (uint32_t)sizeof(ipv6_hdr_t);
    if ((uintptr_t)inner_icmp + sizeof(icmpv6_echo_t)>(uintptr_t)icmp + icmp_len) return false;

    icmpv6_echo_t e;
    memcpy(&e, inner_icmp, sizeof(e));
    if (e.hdr.type != ICMPV6_ECHO_REQUEST) return false;

    if (out_id) *out_id = bswap16(e.id);
    if (out_seq) *out_seq = bswap16(e.seq);
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
        if (icmp_len < sizeof(icmpv6_echo_t)) {
            netpkt_unref(pkt);
            return;
        }
        icmpv6_echo_t e;
        memcpy(&e, icmp, sizeof(e));
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