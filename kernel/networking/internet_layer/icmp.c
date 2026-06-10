#include "networking/internet_layer/icmp.h"
#include "net/checksums.h"
#include "std/std.h"
#include "console/kio.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv4.h"
#include "syscalls/syscalls.h"

#define MAX_PENDING 16

typedef struct {
    bool in_use;
    uint16_t id;
    uint16_t seq;
    bool received;
    uint8_t rx_type;
    uint8_t rx_code;
    uint32_t start_ms;
    uint32_t end_ms;
    uint32_t rx_src_ip;
} ping_slot_t;

static ping_slot_t g_pending[MAX_PENDING] = {0};

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
            g_pending[i].rx_src_ip = 0;
            return i;
        }
    }
    return -1;
}

static void mark_received(uint16_t id, uint16_t seq, uint8_t type, uint8_t code, uint32_t src_ip) {
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g_pending[i].in_use && g_pending[i].id == id && g_pending[i].seq == seq) {
            g_pending[i].received = true;
            g_pending[i].rx_type = type;
            g_pending[i].rx_code = code;
            g_pending[i].end_ms = (uint32_t)get_time();
            g_pending[i].rx_src_ip = src_ip;
            return;
        }
    }
}

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
    pkt->checksum = checksum16((uint16_t*)pkt, (len+1)/2);
    return buf;
}

bool icmp_ping(uint32_t dst_ip, uint16_t id, uint16_t seq, uint32_t timeout_ms, const void* tx_opts_or_null, uint32_t ttl, ping_result_t* out) {
    int slot = alloc_slot(id, seq);
    if (slot < 0) {
        if (out) {
            out->rtt_ms = 0;
            out->status = PING_UNKNOWN_ERROR;
            out->icmp_type = 0xFF;
            out->icmp_code = 0xFF;
            out->responder_ip = 0;
        }
        return false;
    }

    uint32_t tot_len = 0;
    uintptr_t buf = build_echo(id, seq, NULL, 32, &tot_len);
    if (!buf) {
        if (out) {
            out->rtt_ms = 0;
            out->status = PING_UNKNOWN_ERROR;
            out->icmp_type = 0xFF;
            out->icmp_code = 0xFF;
            out->responder_ip = 0;
        }
        g_pending[slot].in_use = false;
        return false;
    }

    uint32_t headroom = (uint32_t)sizeof(eth_hdr_t) + (uint32_t)sizeof(ipv4_hdr_t);
    netpkt_t* pkt = netpkt_alloc(tot_len, headroom, 0);
    if (!pkt) {
        release((void*)buf);
        g_pending[slot].in_use = false;
        return false;
    }
    void* p = netpkt_put(pkt, tot_len);
    if (!p) {
        netpkt_unref(pkt);
        release((void*)buf);
        g_pending[slot].in_use = false;
        return false;
    }
    memcpy(p, (const void*)buf, tot_len);
    release((void*)buf);
    ipv4_send_packet(dst_ip, 1, pkt, (const ip_tx_opts_t*)tx_opts_or_null, (uint8_t)ttl, 0);

    uint32_t start = (uint32_t)get_time();
    for (;;) {
        if (g_pending[slot].received) {
            if (out) {
                out->icmp_type = g_pending[slot].rx_type;
                out->icmp_code = g_pending[slot].rx_code;
                out->responder_ip = g_pending[slot].rx_src_ip;
                switch (g_pending[slot].rx_type) {
                    case ICMP_ECHO_REPLY: out->status = PING_OK; break;
                    case ICMP_DEST_UNREACH:
                        switch (g_pending[slot].rx_code) {
                            case 0: out->status = PING_NET_UNREACH; break;
                            case 1: out->status = PING_HOST_UNREACH; break;
                            case 2: out->status = PING_PROTO_UNREACH; break;
                            case 3: out->status = PING_PORT_UNREACH; break;
                            case 4: out->status = PING_FRAG_NEEDED; break;
                            case 5: out->status = PING_SRC_ROUTE_FAILED; break;
                            case 13: out->status = PING_ADMIN_PROHIBITED; break;
                            default: out->status = PING_UNKNOWN_ERROR; break;
                        }
                        break;
                    case ICMP_TIME_EXCEEDED: out->status = PING_TTL_EXPIRED; break;
                    case ICMP_PARAM_PROBLEM: out->status = PING_PARAM_PROBLEM; break;
                    case ICMP_REDIRECT: out->status = PING_REDIRECT; break;
                    default: out->status = PING_UNKNOWN_ERROR; break;
                }

                if (g_pending[slot].end_ms >= g_pending[slot].start_ms) out->rtt_ms = g_pending[slot].end_ms - g_pending[slot].start_ms;
                else out->rtt_ms = 0;
            }
            bool ok = (g_pending[slot].rx_type == ICMP_ECHO_REPLY);
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
        out->responder_ip = 0;
    }
    g_pending[slot].in_use = false;
    return false;
}

void icmp_input(netpkt_t* pkt, uint32_t src_ip, uint32_t dst_ip) {
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
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)((raw[i] << 8) | raw[i + 1]);
    if (len & 1u) sum += (uint32_t)(raw[len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    if ((uint16_t)sum != 0xFFFFu) {
        netpkt_unref(pkt);
        return;
    }

    uint8_t type = hdr[0];
    uint8_t code = hdr[1];
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
        rp->checksum = checksum16((uint16_t*)rp, (rlen+1)/2);

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
        mark_received(id, sq, type, code, src_ip);
        netpkt_unref(pkt);
        return;
    }

    if (type == ICMP_TIME_EXCEEDED || type == ICMP_DEST_UNREACH || type == ICMP_PARAM_PROBLEM || type == ICMP_REDIRECT) {
        if (pay >= 28) {
        const uint8_t *ip = raw + 8;
        uint8_t ihl = (uint8_t)(ip[0] & 0x0F);
        uint32_t iphdr = (uint32_t)ihl * 4;

        if (pay >= iphdr + 8) {
            uint8_t proto = ip[9];
            if (proto == 1) {
                const uint8_t *ic = raw + 8 + iphdr;
                uint8_t t = ic[0];
                if (t == ICMP_ECHO_REQUEST || t == ICMP_ECHO_REPLY) {
                    uint16_t iid = (uint16_t)((ic[4] << 8) | ic[5]);
                    uint16_t isq = (uint16_t)((ic[6] << 8) | ic[7]);
                    mark_received(iid, isq, type, code, src_ip);
                }
            }
        }
    }
        netpkt_unref(pkt);
        return;
    }

    netpkt_unref(pkt);
}
