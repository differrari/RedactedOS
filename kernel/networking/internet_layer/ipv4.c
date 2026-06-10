#include "ipv4.h"
#include "ipv4_route.h"
#include "networking/link_layer/arp.h"
#include "networking/internet_layer/icmp.h"
#include "networking/internet_layer/igmp.h"
#include "std/memory.h"
#include "std/string.h"
#include "networking/transport_layer/tcp.h"
#include "networking/transport_layer/udp.h"
#include "console/kio.h"
#include "syscalls/syscalls.h"
#include "ipv4_utils.h"
#include "net/network_types.h"
#include "networking/link_layer/nic_types.h"
#include "networking/net_fragbuf.h"

static uint16_t g_ip_ident = 1;

#define IPV4_REASS_SLOTS 8
typedef struct {
    uint8_t used;
    uint8_t ifindex;
    uint16_t ident;
    uint8_t proto;
    uint32_t src;
    uint32_t dst;
    uint32_t last_update_ms;
    net_fragbuf_t frag;
} ipv4_reass_slot_t;

static ipv4_reass_slot_t g_ipv4_reass[IPV4_REASS_SLOTS];

static void ipv4_reass_free(ipv4_reass_slot_t *s) {
    if (!s) return;
    net_fragbuf_free(&s->frag);
    memset(s, 0, sizeof(*s));
}

bool ipv4_send_packet(uint32_t dst_ip, uint8_t proto, netpkt_t* pkt, const ip_tx_opts_t* opts, uint8_t ttl, uint8_t dontfrag) {
    if (!pkt || !netpkt_len(pkt)) {
        if (pkt) netpkt_unref(pkt);
        return false;
    }

    uint8_t ifx = 0;
    uint32_t src_ip = 0;
    uint32_t nh = dst_ip;
    l3_ipv4_interface_t* src_v4 = NULL;

    if (ipv4_is_limited_broadcast(dst_ip)) {
        if (!opts || opts->scope != IP_TX_BOUND_L3) {
            netpkt_unref(pkt);
            return false;
        }

        src_v4 = l3_ipv4_find_by_id(opts->index);
        if (!src_v4 || !src_v4->l2 || !src_v4->l2->is_up || src_v4->mode == IPV4_CFG_DISABLED) {
            netpkt_unref(pkt);
            return false;
        }

        ifx = src_v4->l2->ifindex;
        src_ip = src_v4->ip;
        nh = 0xFFFFFFFF;
    } else {
        ipv4_tx_plan_t plan;
        if (!ipv4_build_tx_plan(dst_ip, opts, &plan)) {
            netpkt_unref(pkt);
            return false;
        }

        src_v4 = l3_ipv4_find_by_id(plan.l3_id);
        if (!src_v4 || !src_v4->l2 || !src_v4->l2->is_up || src_v4->mode == IPV4_CFG_DISABLED || !src_v4->ip) {
            netpkt_unref(pkt);
            return false;
        }

        ifx = src_v4->l2->ifindex;
        src_ip = plan.src_ip;

        uint32_t route_nh = 0;
        bool have_nh = false;

        if (src_v4->routing_table && ipv4_rt_lookup_in(src_v4->routing_table, dst_ip, &route_nh, NULL, NULL)) {
            nh = route_nh ? route_nh : dst_ip;
            have_nh = true;
        }
        if (!have_nh && src_v4->mask && ipv4_net(dst_ip, src_v4->mask) == ipv4_net(src_v4->ip, src_v4->mask)) {
            nh = dst_ip;
            have_nh = true;
        }
        if (!have_nh && src_v4->gw) {
            nh = src_v4->gw;
            have_nh = true;
        }
        if (!have_nh) {
            netpkt_unref(pkt);
            return false;
        }
    }

    l2_interface_t* l2 = src_v4->l2;
    uint8_t dst_mac[6];
    bool need_arp = false;
    bool is_dbcast = src_v4->mask && nh == dst_ip && ipv4_broadcast_calc(src_v4->ip, src_v4->mask) == dst_ip;

    if (ipv4_is_limited_broadcast(dst_ip) || is_dbcast) memset(dst_mac, 0xFF, 6);
    else if (ipv4_is_multicast(dst_ip)) ipv4_mcast_to_mac(dst_ip, dst_mac);
    else if (l2->kind == NET_IFK_LOCALHOST) memset(dst_mac, 0, 6);
    else need_arp = true;

    uint16_t mtu = src_v4->runtime_opts_v4.mtu ? src_v4->runtime_opts_v4.mtu : 1500;
    uint32_t hdr_len = IP_IHL_NOOPTS * 4;
    uint32_t seg_len = netpkt_len(pkt);
    uint32_t total = hdr_len + seg_len;

    if (total <= (uint32_t)mtu) {
        void* hdrp = netpkt_push(pkt, hdr_len);
        if (!hdrp) {
            netpkt_unref(pkt);
            return false;
        }

        uint16_t ip_[sizeof(ipv4_hdr_t)/sizeof(uint16_t)];
        ipv4_hdr_t* ip = (ipv4_hdr_t*)ip_;
        ip->version_ihl = (uint8_t)((IP_VERSION_4 << 4) | IP_IHL_NOOPTS);
        ip->dscp_ecn = 0;
        ip->total_length = bswap16((uint16_t)total);
        ip->identification = bswap16(g_ip_ident++);
        uint16_t ff = 0;
        if (dontfrag) ff |= 0x4000;
        ip->flags_frag_offset = bswap16(ff);
        ip->ttl = ttl ? ttl : IP_TTL_DEFAULT;
        ip->protocol = proto;
        ip->header_checksum = 0;
        ip->src_ip = bswap32(src_ip);
        ip->dst_ip = bswap32(dst_ip);
        ip->header_checksum = checksum16(ip_, hdr_len / 2);
        memcpy(hdrp, ip, sizeof(*ip));

        if (need_arp) return arp_send_or_queue_on(ifx, nh, pkt);
        return eth_send_frame_on(ifx, ETHERTYPE_IPV4, dst_mac, pkt);
    }

    if (dontfrag || (uint32_t)mtu < hdr_len + 8) {
        netpkt_unref(pkt);
        return false;
    }

    uint32_t max_chunk = ((uint32_t)mtu - hdr_len) / 8 * 8;
    if (!max_chunk) {
        netpkt_unref(pkt);
        return false;
    }

    uint16_t ident = g_ip_ident++;
    uint32_t off = 0;
    const uint8_t* data = (const uint8_t*)netpkt_data(pkt);
    bool ok = true;

    while (off < seg_len) {
        uint32_t remain = seg_len - off;
        uint32_t chunk = remain > max_chunk ? max_chunk : remain;
        uint8_t more = off + chunk < seg_len ? 1 : 0;
        uint32_t frame_len = hdr_len + chunk;

        netpkt_t* fpkt = netpkt_alloc(frame_len, sizeof(eth_hdr_t), 0);
        if (!fpkt) {
            ok = false;
            break;
        }

        void* buf = netpkt_put(fpkt, frame_len);
        if (!buf) {
            netpkt_unref(fpkt);
            ok = false;
            break;
        }

        uint16_t ip_[sizeof(ipv4_hdr_t) / sizeof(uint16_t)];
        ipv4_hdr_t* ip = (ipv4_hdr_t*)ip_;
        ip->version_ihl = (uint8_t)((IP_VERSION_4 << 4) | IP_IHL_NOOPTS);
        ip->dscp_ecn = 0;
        ip->total_length = bswap16((uint16_t)frame_len);
        ip->identification = bswap16(ident);
        uint16_t ff = (uint16_t)((off / 8) & 0x1FFF);
        if (more) ff |= 0x2000;
        ip->flags_frag_offset = bswap16(ff);
        ip->ttl = ttl ? ttl : IP_TTL_DEFAULT;
        ip->protocol = proto;
        ip->header_checksum = 0;
        ip->src_ip = bswap32(src_ip);
        ip->dst_ip = bswap32(dst_ip);
        ip->header_checksum = checksum16(ip_, hdr_len / 2);

        memcpy(buf, ip, sizeof(*ip));
        memcpy((uint8_t*)buf + hdr_len, data + off, chunk);

        if (need_arp) {
            if (!arp_send_or_queue_on(ifx, nh, fpkt)) ok = false;
        } else if (!eth_send_frame_on(ifx, ETHERTYPE_IPV4, dst_mac, fpkt)) ok = false;

        off += chunk;
    }

    netpkt_unref(pkt);
    return ok && off == seg_len;
}

static void ipv4_deliver_l4(uint16_t ifindex, netpkt_t* pkt, uint32_t l4_off, uint32_t l4_len, uint8_t proto, uint32_t src, uint32_t dst) {
    l2_interface_t* l2 = l2_interface_find_by_index((uint8_t)ifindex);
    if (!l2) return;

    l3_ipv4_interface_t* cand[MAX_IPV4_PER_INTERFACE];
    int ccount = 0;
    for (int s = 0; s < MAX_IPV4_PER_INTERFACE; ++s) {
        l3_ipv4_interface_t* v4 = l2->l3_v4[s];
        if (!v4) continue;
        if (v4->mode == IPV4_CFG_DISABLED) continue;
        cand[ccount++] = v4;
    }

    if (ccount == 0) return;
    if (ipv4_is_multicast(dst)) {
        bool joined = false;
        for (int i = 0; i < (int)l2->ipv4_mcast_count; ++i) {
            if (l2->ipv4_mcast[i] == dst) {
                joined = true;
                break;
            }
        }
        if (!joined) return;

        for (int i = 0; i < ccount; ++i) {
            netpkt_t* l4pkt = netpkt_view(pkt, l4_off, l4_len);
            if (!l4pkt) continue;
            uint8_t l3id = cand[i]->l3_id;
            if (proto == PROTO_IGMP) igmp_input((uint8_t)ifindex, src, dst, l4pkt);
            else if (proto == PROTO_TCP) tcp_input(IP_VER4, &src, &dst, l3id, l4pkt);
            else if (proto == PROTO_UDP) udp_input(IP_VER4, &src, &dst, l3id, l4pkt);
            else netpkt_unref(l4pkt);
        }
        return;
    }

    if (dst == 0xFFFFFFFF) {
        for (int i = 0; i < ccount; ++i) {
            netpkt_t* l4pkt = netpkt_view(pkt, l4_off, l4_len);
            if (!l4pkt) continue;
            uint8_t l3id = cand[i]->l3_id;
            if (proto == PROTO_ICMP) icmp_input(l4pkt, src, dst);
            else if (proto == PROTO_IGMP) igmp_input((uint8_t)ifindex, src, dst, l4pkt);
            else if (proto == PROTO_TCP) tcp_input(IP_VER4, &src, &dst, l3id, l4pkt);
            else if (proto == PROTO_UDP) udp_input(IP_VER4, &src, &dst, l3id, l4pkt);
            else netpkt_unref(l4pkt);
        }
        return;
    }

    int match_count = 0;
    uint8_t match_l3id = 0;
    for (int i = 0; i < ccount; ++i) {
        if (cand[i]->ip && cand[i]->ip == dst) {
            match_count++;
            if (match_count == 1) match_l3id = cand[i]->l3_id;
        }
    }

    if (match_count == 1) {
        netpkt_t* l4pkt = netpkt_view(pkt, l4_off, l4_len);
        if (!l4pkt) return;
        if (proto == PROTO_ICMP) icmp_input(l4pkt, src, dst);
        else if (proto == PROTO_IGMP) igmp_input((uint8_t)ifindex, src, dst, l4pkt);
        else if (proto == PROTO_TCP) tcp_input(IP_VER4, &src, &dst, match_l3id, l4pkt);
        else if (proto == PROTO_UDP) udp_input(IP_VER4, &src, &dst, match_l3id, l4pkt);
        else netpkt_unref(l4pkt);
        return;
    }

    if (match_count > 1) {
        for (int i = 0; i < ccount; ++i) {
            if (cand[i]->ip != dst) continue;

            netpkt_t* l4pkt = netpkt_view(pkt, l4_off, l4_len);
            if (!l4pkt) continue;
            uint8_t l3id = cand[i]->l3_id;
            if (proto == PROTO_ICMP) icmp_input(l4pkt, src, dst);
            else if (proto == PROTO_IGMP) igmp_input((uint8_t)ifindex, src, dst, l4pkt);
            else if (proto == PROTO_TCP) tcp_input(IP_VER4, &src, &dst, l3id, l4pkt);
            else if (proto == PROTO_UDP) udp_input(IP_VER4, &src, &dst, l3id, l4pkt);
            else netpkt_unref(l4pkt);
        }
        return;
    }

    for (int i = 0; i < ccount; ++i) {
        l3_ipv4_interface_t* v4 = cand[i];
        if (!v4 || !v4->ip || !v4->mask) continue;
        if (v4->is_localhost) continue;
        if (ipv4_broadcast_calc(v4->ip, v4->mask) != dst) continue;

        netpkt_t* l4pkt = netpkt_view(pkt, l4_off, l4_len);
        if (!l4pkt) return;
        if (proto == PROTO_ICMP) icmp_input(l4pkt, src, dst);
        else if (proto == PROTO_TCP) tcp_input(IP_VER4, &src, &dst, v4->l3_id, l4pkt);
        else if (proto == PROTO_UDP) udp_input(IP_VER4, &src, &dst, v4->l3_id, l4pkt);
        else netpkt_unref(l4pkt);
        return;
    }
}

void ipv4_input(uint16_t ifindex, netpkt_t* pkt, const uint8_t src_mac[6]) {
    if (!pkt) return;
    uint32_t ip_len = netpkt_len(pkt);
    if (ip_len < sizeof(ipv4_hdr_t)) return;

    uint8_t first = 0;
    if (!netpkt_copyout(pkt, 0, &first, sizeof(first))) return;
    uint8_t ver = (uint8_t)(first >> 4);
    uint8_t ihl = (uint8_t)(first & 0x0F);
    if (ver != IP_VERSION_4) return;
    if (ihl < IP_IHL_NOOPTS) return;

    uint32_t hdr_len = (uint32_t)ihl * 4;
    if (ip_len < hdr_len) return;

    uint16_t hdr_words[60 /sizeof(uint16_t)];
    uint8_t* hdr_copy = (uint8_t*)hdr_words;
    if (!netpkt_copyout(pkt, 0, hdr_copy, hdr_len)) return;
    if (checksum16(hdr_words, hdr_len / 2) != 0) return;
    ipv4_hdr_t ip;
    memcpy(&ip, hdr_copy, sizeof(ip));

    uint16_t ip_totlen = bswap16(ip.total_length);
    if (ip_totlen < hdr_len) return;
    if (ip_len < ip_totlen) return;
    (void)netpkt_trim(pkt, ip_totlen);

    uint32_t src = bswap32(ip.src_ip);
    uint32_t dst = bswap32(ip.dst_ip);

    if (ifindex && src && src_mac) {
        uint8_t mac_old[6];
        bool had = arp_table_get_for_l2((uint8_t)ifindex, src, mac_old);
        if (!had || memcmp(mac_old, src_mac, 6) != 0) arp_table_put_for_l2((uint8_t)ifindex, src, src_mac, 180000, false);
        else arp_table_put_for_l2((uint8_t)ifindex, src, mac_old, 180000, false);
    }

    uint8_t proto = ip.protocol;
    uint32_t l4_len = (uint32_t)ip_totlen - hdr_len;
    uint16_t ff = bswap16(ip.flags_frag_offset);
    uint32_t off = (uint32_t)(ff & 0x1FFF) * 8;
    uint8_t more = (ff & 0x2000) ? 1 : 0;

    if (off || more) {
        if (!l4_len) return;
        if (more && (l4_len & 7)) return;
        if (off + l4_len > NET_FRAGBUF_DEFAULT_MAX_LEN) return;

        uint32_t now = (uint32_t)get_time();
        ipv4_reass_slot_t* slot = NULL;
        uint16_t ident = bswap16(ip.identification);

        for (int i = 0; i < IPV4_REASS_SLOTS; i++) {
            ipv4_reass_slot_t* s = &g_ipv4_reass[i];
            if (!s->used) continue;
            if (now - s->last_update_ms > 60000) {
                ipv4_reass_free(s);
                continue;
            }
            if (s->ifindex == (uint8_t)ifindex && s->ident == ident && s->proto == proto && s->src == src && s->dst == dst) slot = s;
        }

        if (!slot) {
            for (int i = 0; i < IPV4_REASS_SLOTS; i++) {
                if (g_ipv4_reass[i].used) continue;
                slot = &g_ipv4_reass[i];
                memset(slot, 0, sizeof(*slot));
                slot->used = 1;
                net_fragbuf_init(&slot->frag);
                slot->ifindex = (uint8_t)ifindex;
                slot->ident = ident;
                slot->proto = proto;
                slot->src = src;
                slot->dst = dst;
                slot->last_update_ms = now;
                break;
            }
        }

        if (!slot) return;
        if (!net_fragbuf_add(&slot->frag, pkt, hdr_len, off, l4_len, more)) {
            ipv4_reass_free(slot);
            return;
        }

        slot->last_update_ms = now;

        if (!net_fragbuf_complete(&slot->frag)) return;

        netpkt_t* reassembled = net_fragbuf_take_packet(&slot->frag);
        if (!reassembled) {
            ipv4_reass_free(slot);
            return;
        }

        ipv4_reass_free(slot);
        ipv4_deliver_l4(ifindex, reassembled, 0, netpkt_len(reassembled), proto, src, dst);
        netpkt_unref(reassembled);
        return;
    }

    ipv4_deliver_l4(ifindex, pkt, hdr_len, l4_len, proto, src, dst);
}
