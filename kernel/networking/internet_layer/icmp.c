#include "networking/internet_layer/icmp.h"
#include "net/checksums.h"
#include "std/std.h"
#include "console/kio.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/transport_layer/csocket.h"
#include "networking/transport_layer/csocket_raw.h"
#include "networking/transport_layer/trans_utils.h"
#include "syscalls/syscalls.h"

static uintptr_t build_echo(uint16_t id, uint16_t seq, const uint8_t* payload, uint32_t pay_len, uint32_t* out_total_len) {
    uint32_t len = 8 + (pay_len > 56 ? 56 : pay_len);
    *out_total_len = len;
    uintptr_t buf = (uintptr_t)zalloc(8 + 56);
    if (!buf) return 0;

    icmp_packet *pkt = (icmp_packet*)buf;
    pkt->type = ICMP_ECHO_REQUEST;
    pkt->code = 0;
    pkt->id = bswap16(id);
    pkt->seq = bswap16(seq);

    memset(pkt->payload, 0, 56);
    if (payload && pay_len) memcpy(pkt->payload, payload, (pay_len > 56 ? 56 : pay_len));
    pkt->checksum = 0;
    pkt->checksum = bswap16(checksum16(pkt, len));
    return buf;
}

uint32_t icmp_ping_collect(uint32_t dst_ip, uint16_t id, uint16_t seq, uint32_t timeout_ms, const void* tx_opts_or_null, uint32_t ttl, ping_result_t* out, uint32_t max_results) {
    if (!out || !max_results) return 0;

    uint32_t tot_len = 0;
    uintptr_t buf = build_echo(id, seq, NULL, 32, &tot_len);
    if (!buf) return 0;

    SocketOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.special_kind = SOCKET_SPECIAL_RAW;
    if (ttl) {
        opt.flags |= SOCK_OPT_TTL;
        opt.ttl = (uint8_t)ttl;
    }
    socket_handle_t sock = create_socket(PROTO_ICMP, &opt);
    if (!sock) {
        release((void*)buf);
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
            bind.ver = IP_VER4;
            bind.l3_id = tx->index;
        }
        if (bind.kind && bind_socket(sock, &bind, 0) != SOCK_OK) {
            close_socket(sock);
            release((void*)buf);
            return 0;
        }
    }

    net_l4_endpoint dst;
    make_ep(&dst_ip, 0, IP_VER4, &dst);
    if (send_to_socket(sock, &dst, (const void*)buf, tot_len) != (int64_t)tot_len) {
        close_socket(sock);
        release((void*)buf);
        return 0;
    }
    release((void*)buf);

    uint32_t count = 0;
    uint32_t start = (uint32_t)get_time();
    while (count < max_results) {
        uint32_t now = (uint32_t)get_time();
        if (now - start >= timeout_ms) break;

        uint8_t rx[576];
        net_l4_endpoint src;
        memset(&src, 0, sizeof(src));
        int64_t n = receive_from_socket(sock, rx, sizeof(rx), &src);
        if (n == SOCK_ERR_WOULDBLOCK) {
            msleep(5);
            continue;
        }

        if (n < 8) {
        if (n < 0) msleep(5);
        continue;
        }

        uint8_t type = rx[0];
        uint8_t code = rx[1];
        bool matched = false;
        if (type == ICMP_ECHO_REPLY && rd_be16(rx + 4) == id && rd_be16(rx + 6) == seq) matched = true;
        else if ((type == ICMP_TIME_EXCEEDED || type == ICMP_DEST_UNREACH || type == ICMP_PARAM_PROBLEM || type == ICMP_REDIRECT) && n >= 36) {
            const uint8_t* ip = rx + 8;
            uint8_t ihl = (uint8_t)(ip[0] & 0x0F);
            uint32_t iphdr = (uint32_t)ihl * 4;
            if (ihl >= IP_IHL_NOOPTS && (uint32_t)n >= 8 + iphdr + 8 && ip[9] == PROTO_ICMP) {
                const uint8_t* ic = rx + 8 + iphdr;
                uint8_t inner_type = ic[0];
                if ((inner_type == ICMP_ECHO_REQUEST || inner_type == ICMP_ECHO_REPLY) && rd_be16(ic + 4) == id && rd_be16(ic + 6) == seq) matched = true;
            }
        }
        if (!matched) continue;

        ping_result_t* r = &out[count++];
        memset(r, 0, sizeof(*r));
        r->icmp_type = type;
        r->icmp_code = code;
        memcpy(&r->responder_ip, src.ip, sizeof(r->responder_ip));
        now = (uint32_t)get_time();
        r->rtt_ms = now >= start ? now - start : 0;
        switch (type) {
            case ICMP_ECHO_REPLY:
                r->status = PING_OK;
                break;
            case ICMP_DEST_UNREACH:
                switch (code) {
                    case 0: r->status = PING_NET_UNREACH; break;
                    case 1: r->status = PING_HOST_UNREACH; break;
                    case 2: r->status = PING_PROTO_UNREACH; break;
                    case 3: r->status = PING_PORT_UNREACH; break;
                    case 4: r->status = PING_FRAG_NEEDED; break;
                    case 5: r->status = PING_SRC_ROUTE_FAILED; break;
                    case 13: r->status = PING_ADMIN_PROHIBITED; break;
                    default: r->status = PING_UNKNOWN_ERROR; break;
                }
                break;
            case ICMP_TIME_EXCEEDED: r->status = PING_TTL_EXPIRED; break;
            case ICMP_PARAM_PROBLEM: r->status = PING_PARAM_PROBLEM; break;
            case ICMP_REDIRECT: r->status = PING_REDIRECT; break;
            default: r->status = PING_UNKNOWN_ERROR; break;
        }
        if (max_results == 1) break;
    }

    close_socket(sock);
    return count;
}

bool icmp_ping(uint32_t dst_ip, uint16_t id, uint16_t seq, uint32_t timeout_ms, const void* tx_opts_or_null, uint32_t ttl, ping_result_t* out) {
    ping_result_t res;
    ping_result_t *dst = out ? out : &res;
    uint32_t n = icmp_ping_collect(dst_ip, id, seq, timeout_ms, tx_opts_or_null, ttl, dst, 1);
    return n && dst->status == PING_OK;
}

void icmp_input(uint8_t ifindex, netpkt_t* pkt, uint32_t src_ip, uint32_t dst_ip) {
    if (!pkt) return;

    uint32_t len = netpkt_len(pkt);
    if (len < 8) {
        netpkt_unref(pkt);
        return;
    }

    const uint8_t* raw = (const uint8_t*)netpkt_data(pkt);
    uint8_t hdr[8];
    if (!netpkt_copyout(pkt, 0, hdr, sizeof(hdr))) {
        netpkt_unref(pkt);
        return;
    }
    if (checksum16(raw, len) != 0) {
        netpkt_unref(pkt);
        return;
    }

    socket_raw_input_v4(PROTO_ICMP, ifindex, src_ip, dst_ip, pkt);
    uint8_t type = hdr[0];
    uint16_t id = rd_be16(hdr + 4);
    uint16_t sq = rd_be16(hdr + 6);
    uint32_t pay = len - 8;
    if (pay > 56) pay = 56;

    if (type == ICMP_ECHO_REQUEST) {
        uintptr_t buf = (uintptr_t)zalloc(8 + 56);
        if (!buf) {
            netpkt_unref(pkt);
            return;
        }
        icmp_packet *rp = (icmp_packet*)buf;
        rp->type = ICMP_ECHO_REPLY;
        rp->code = 0;
        rp->id = bswap16(id);
        rp->seq = bswap16(sq);
        memset(rp->payload, 0, 56);
        if (pay) memcpy(rp->payload, raw + 8, pay);
        rp->checksum = 0;
        uint32_t rlen = 8 + pay;
        rp->checksum = bswap16(checksum16(rp, rlen));

        l3_ipv4_interface_t* l3 = l3_ipv4_find_by_ip(dst_ip);
        if (l3 && l3->l2) {
            ip_tx_opts_t o = {.index = l3->l3_id, .scope = IP_TX_BOUND_L3};
            uint32_t headroom = (uint32_t)sizeof(eth_hdr_t) + (uint32_t)sizeof(ipv4_hdr_t);
            netpkt_t* pkt = netpkt_alloc(rlen, headroom, 0);
            if (pkt) {
                void* p = netpkt_put(pkt, rlen);
                if (p) {
                    memcpy(p, (const void*)buf, rlen);
                    ipv4_send_packet(src_ip, 1, pkt, &o, IP_TTL_DEFAULT, 0);
                } else {
                    netpkt_unref(pkt);
                }
            }
        }
        release((void*)buf);
        netpkt_unref(pkt);
        return;
    }

    if (type == ICMP_ECHO_REPLY) {
        netpkt_unref(pkt);
        return;
    }

    if (type == ICMP_TIME_EXCEEDED || type == ICMP_DEST_UNREACH || type == ICMP_PARAM_PROBLEM || type == ICMP_REDIRECT) {
        netpkt_unref(pkt);
        return;
    }

    netpkt_unref(pkt);
}
