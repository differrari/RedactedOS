#include "networking/internet_layer/icmp.h"
#include "net/checksums.h"
#include "std/std.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/transport_layer/csocket_raw.h"
#include "networking/transport_layer/trans_utils.h"

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
