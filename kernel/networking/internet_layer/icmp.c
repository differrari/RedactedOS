#include "networking/internet_layer/icmp.h"
#include "net/checksums.h"
#include "std/std.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/transport_layer/csocket_raw.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/transport_layer/tcp.h"
#include "networking/internet_layer/pmtu.h"
#include "networking/internet_layer/ipv4_utils.h"

void icmp_send_port_unreachable(l3_id_t l3_id, uint32_t remote_ip, const uint8_t *ip_header, uint8_t ip_header_len, const netpkt_t *l4pkt, uint32_t l4_off, uint32_t l4_len) {
    if (!l3_id || !remote_ip || !ip_header || !l4pkt) return;
    if (ip_header_len < sizeof(ipv4_hdr_t) || ip_header_len > 60) return;
    if (ipv4_is_unspecified(remote_ip) || ipv4_is_multicast(remote_ip) || ipv4_is_limited_broadcast(remote_ip)) return;

    l3_ipv4_interface_t *l3 = l3_ipv4_find_by_id(l3_id);
    if (!ipv4_l3_is_ready(l3)) return;
    if (l3->mask && ipv4_is_directed_broadcast(l3->ip, l3->mask, remote_ip)) return;

    uint32_t quote_l4 = l4_len < 8 ? l4_len : 8;
    uint32_t icmp_len = 8u + ip_header_len + quote_l4;
    uint32_t headroom = (uint32_t)sizeof(eth_hdr_t) + (uint32_t)sizeof(ipv4_hdr_t);
    netpkt_t *reply = netpkt_alloc(icmp_len, headroom, 0);
    if (!reply) return;
    uint8_t *out = (uint8_t*)netpkt_put(reply, icmp_len);
    if (!out) {
        netpkt_unref(reply);
        return;
    }

    memset(out, 0, 8);
    out[0] = ICMP_DEST_UNREACH;
    out[1] = 3;
    memcpy(out + 8, ip_header, ip_header_len);
    if (quote_l4 && !netpkt_copyout(l4pkt, l4_off, out + 8 + ip_header_len, quote_l4)) {
        netpkt_unref(reply);
        return;
    }
    wr_be16(out + 2u, checksum16(out, icmp_len));

    ip_tx_opts_t tx = {.target = {.l3_id = l3_id}, .scope = IP_TX_BOUND_L3};
    ipv4_send_packet(remote_ip, PROTO_ICMP, reply, &tx, IP_TTL_DEFAULT, 0, 0);
}


static void icmp_note_frag_needed(uint8_t ifindex, const uint8_t* raw, uint32_t len) {
    if (!raw || len < 8 + sizeof(ipv4_hdr_t)) return;

    const uint8_t* quoted = raw + 8;
    uint8_t version_ihl = quoted[0];
    if ((version_ihl >> 4) != 4) return;

    uint32_t ihl = (uint32_t)(version_ihl & 0x0F) * 4;
    if (ihl < sizeof(ipv4_hdr_t) || 8 + ihl > len) return;

    ipv4_hdr_t inner;
    memcpy(&inner, quoted, sizeof(inner));
    uint32_t local_ip = bswap32(inner.src_ip);
    uint32_t remote_ip = bswap32(inner.dst_ip);
    l3_ipv4_interface_t* l3 = l3_ipv4_find_by_ip(local_ip);
    if (!ipv4_l3_is_ready(l3) || !l3->l2 || l3->l2->ifindex != ifindex) return;
    if (!remote_ip || ipv4_is_multicast(remote_ip) || ipv4_is_limited_broadcast(remote_ip)) return;

    uint16_t mtu = rd_be16(raw + 6);
    if (!mtu) {
        static const uint16_t plateau[] = {65535, 32000, 17914, 8166, 4352, 2002, 1492, 1006, 508, 296, 68};
        uint16_t original_len = bswap16(inner.total_length);
        if (original_len < ihl) return;

        mtu = 68;
        for (int i = 0; i < (int)N_ARR(plateau); i++) {
            if (plateau[i] >= original_len) continue;
            mtu = plateau[i];
            break;
        }
    }
    if (mtu < 68) return;

    uint16_t base_mtu = l3_ipv4_effective_mtu(l3);
    if (!base_mtu || mtu >= base_mtu) return;
    uint16_t path_mtu = pmtu_note(l3->l3_id, l3->epoch, IP_VER4, &remote_ip, mtu);
    if (!path_mtu || inner.protocol != PROTO_TCP || len < 8 + ihl + 4) return;

    const uint8_t* tcp = quoted + ihl;
    tcp_pmtu_update(l3->l3_id, IP_VER4, &local_ip, &remote_ip, rd_be16(tcp), rd_be16(tcp + 2), path_mtu);
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

    if (type == ICMP_ECHO_REQUEST) {
        l3_ipv4_interface_t* l3 = l3_ipv4_find_by_ip(dst_ip);
        if (l3 && l3->l2) {
            ip_tx_opts_t o = {.target = {.l3_id = l3->l3_id}, .scope = IP_TX_BOUND_L3};
            uint32_t headroom = (uint32_t)sizeof(eth_hdr_t) + (uint32_t)sizeof(ipv4_hdr_t);
            netpkt_t* reply = netpkt_alloc(len, headroom, 0);
            if (reply) {
                void* out = netpkt_put(reply, len);
                if (out) {
                    memcpy(out, raw, len);
                    icmp_echo_hdr_t* rp = (icmp_echo_hdr_t*)out;
                    rp->type = ICMP_ECHO_REPLY;
                    rp->code = 0;
                    rp->checksum = 0;
                    rp->checksum = bswap16(checksum16(out, len));
                    ipv4_send_packet(src_ip, PROTO_ICMP, reply, &o, IP_TTL_DEFAULT, 0, 0);
                } else {
                    netpkt_unref(reply);
                }
            }
        }
        netpkt_unref(pkt);
        return;
    }

    if (type == ICMP_ECHO_REPLY) {
        netpkt_unref(pkt);
        return;
    }

    if (type == ICMP_DEST_UNREACH) {
        if (hdr[1] == 4) icmp_note_frag_needed(ifindex, raw, len);
        netpkt_unref(pkt);
        return;
    }

    if (type == ICMP_TIME_EXCEEDED || type == ICMP_PARAM_PROBLEM || type == ICMP_REDIRECT) {
        netpkt_unref(pkt);
        return;
    }

    netpkt_unref(pkt);
}
