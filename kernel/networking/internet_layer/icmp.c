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
    uintptr_t buf = (uintptr_t)malloc(8 + 56);
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
        free_sized((void*)buf, 8 + 56);
        g_pending[slot].in_use = false;
        return false;
    }
    void* p = netpkt_put(pkt, tot_len);
    if (!p) {
        netpkt_unref(pkt);
        free_sized((void*)buf, 8 + 56);
        g_pending[slot].in_use = false;
        return false;
    }
    memcpy(p, (const void*)buf, tot_len);
    free_sized((void*)buf, 8 + 56);
    ipv4_send_packet(dst_ip, 1, pkt, (const ipv4_tx_opts_t*)tx_opts_or_null, (uint8_t)ttl, 0);

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

void icmp_input(uintptr_t ptr, uint32_t len, uint32_t src_ip, uint32_t dst_ip) {
    if (len < 8) return;

    icmp_packet* pkt = (icmp_packet*)ptr;
    uint16_t recv_ck = pkt->checksum;
    pkt->checksum = 0;
    uint16_t calc = checksum16((uint16_t*)pkt, (len+1)/2);
    pkt->checksum = recv_ck;
    if (calc != recv_ck) return;

    uint8_t type = pkt->type;
    uint8_t code = pkt->code;
    uint16_t id = bswap16(pkt->id);
    uint16_t sq = bswap16(pkt->seq);
    uint32_t pay = len - 8;
    if (pay > 56) pay = 56;

    if (type == ICMP_ECHO_REQUEST) {
        uintptr_t buf = (uintptr_t)malloc(8 + 56);
        if (!buf) return;
        icmp_packet *rp = (icmp_packet*)buf;
        rp->type = ICMP_ECHO_REPLY;
        rp->code = 0;
        rp->id = bswap16(id);
        rp->seq = bswap16(sq);
        memset(rp->payload, 0, 56);
        if (pay) memcpy(rp->payload, pkt->payload, pay);
        rp->checksum = 0;
        uint32_t rlen = 8 + pay;
        rp->checksum = checksum16((uint16_t*)rp, (rlen+1)/2);

        l3_ipv4_interface_t* l3 = l3_ipv4_find_by_ip(dst_ip);
        if (l3 && l3->l2) {
            ipv4_tx_opts_t o = {.index = l3->l3_id, .scope = IP_TX_BOUND_L3};
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
        free_sized((void*)buf, 8 + 56);
        return;
    }

    if (type == ICMP_ECHO_REPLY) {
        mark_received(id, sq, type, code, src_ip);
        return;
    }

    if (type == ICMP_TIME_EXCEEDED || type == ICMP_DEST_UNREACH || type == ICMP_PARAM_PROBLEM || type == ICMP_REDIRECT) {
        if (pay >= 28) {
        const uint8_t *ip = pkt->payload;
        uint8_t ihl = (uint8_t)(ip[0] & 0x0F);
        uint32_t iphdr = (uint32_t)ihl * 4;

        if (pay >= iphdr + 8) {
            uint8_t proto = ip[9];
            if (proto == 1) {
                const uint8_t *ic = pkt->payload + iphdr;
                uint8_t t = ic[0];
                if (t == ICMP_ECHO_REQUEST || t == ICMP_ECHO_REPLY) {
                    uint16_t iid = (uint16_t)((ic[4] << 8) | ic[5]);
                    uint16_t isq = (uint16_t)((ic[6] << 8) | ic[7]);
                    mark_received(iid, isq, type, code, src_ip);
                }
            }
        }
    }
        return;
    }
}
