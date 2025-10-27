#include "ipv4.h"
#include "ipv4_route.h"
#include "net/link_layer/arp.h"
#include "net/internet_layer/icmp.h"
#include "std/memory.h"
#include "std/string.h"
#include "net/transport_layer/tcp.h"
#include "net/transport_layer/udp.h"
#include "console/kio.h"
#include "syscalls/syscalls.h"

static uint16_t g_ip_ident = 1;

static int mask_prefix_len(uint32_t m) {
    int n = 0;
    while (m & 0x80000000u) { n++; m <<= 1; }
    return n;
}

static inline bool is_lbcast(uint32_t ip) { return ip == 0xFFFFFFFFu; }
static inline bool is_mcast(uint32_t ip) { return (ip & 0xF0000000u) == 0xE0000000u; }
static inline bool is_directed_bcast_for(const l3_ipv4_interface_t* v4, uint32_t dst) {
    if (!v4) return false;
    if (!v4->mask) return false;
    uint32_t b = ipv4_broadcast_calc(v4->ip, v4->mask);
    return b == dst;
}
static l3_ipv4_interface_t* best_v4_on_l2_for_dst(l2_interface_t* l2, uint32_t dst) {
    l3_ipv4_interface_t* best = NULL;
    int best_pl = -1;
    for (int s = 0; s < MAX_IPV4_PER_INTERFACE; s++) {
        l3_ipv4_interface_t* v4 = l2->l3_v4[s];
        if (!v4) continue;
        if (v4->mode == IPV4_CFG_DISABLED) continue;
        if (!v4->ip) continue;
        uint32_t m = v4->mask;
        if (m && ((dst & m) == (v4->ip & m))) {
            int pl = mask_prefix_len(m);
            if (pl > best_pl) { best_pl = pl; best = v4; }
        } else if (!best) {
            best = v4;
        }
    }
    return best;
}

static bool lookup_route_in_tables(uint32_t dst, uint32_t* out_nh, uint8_t* out_ifx, uint32_t* out_src) {
    int best_pl = -1;
    int best_metric = 0x7FFF;
    uint32_t best_nh = 0;
    uint8_t best_ifx = 0;
    uint32_t best_src = 0;

    uint8_t cnt = l2_interface_count();
    for (uint8_t i = 0; i < cnt; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2) continue;
        for (int s = 0; s < MAX_IPV4_PER_INTERFACE; s++) {
            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
            if (!v4) continue;
            if (v4->mode == IPV4_CFG_DISABLED) continue;
            if (!v4->routing_table) continue;

            uint32_t nh = 0; int pl = -1; int metric = 0;
            if (ipv4_rt_lookup_in(v4->routing_table, dst, &nh, &pl, &metric)) {
                if (pl > best_pl || (pl == best_pl && metric < best_metric)) {
                    best_pl = pl;
                    best_metric = metric;
                    best_nh = nh ? nh : dst;
                    best_ifx = l2->ifindex;
                    best_src = v4->ip;
                }
            }
        }
    }

    if (best_pl >= 0) {
        if (out_nh) *out_nh = best_nh;
        if (out_ifx) *out_ifx = best_ifx;
        if (out_src) *out_src = best_src;
        return true;
    }
    return false;
}

static bool pick_broadcast_bound_l3(uint8_t l3_id, uint8_t* out_ifx, uint32_t* out_src, uint32_t* out_nh) {
    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(l3_id);
    if (!v4 || !v4->l2) return false;
    if (v4->mode == IPV4_CFG_DISABLED) return false;

    if (out_ifx) *out_ifx = v4->l2->ifindex;
    if (out_src) *out_src = v4->ip;
    if (!v4->ip && v4->mode == IPV4_CFG_DHCP && out_src) *out_src = 0;
    if (out_nh) *out_nh = 0xFFFFFFFFu;
    return true;
}

static bool pick_broadcast_bound_l2(uint8_t ifindex, uint8_t* out_ifx, uint32_t* out_src, uint32_t* out_nh) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return false;

    l3_ipv4_interface_t* chosen = NULL;
    l3_ipv4_interface_t* dhcp = NULL;
    for (int s = 0; s < MAX_IPV4_PER_INTERFACE; s++) {
        l3_ipv4_interface_t* v4 = l2->l3_v4[s];
        if (!v4) continue;
        if (v4->mode == IPV4_CFG_DISABLED) continue;
        if (v4->ip) { chosen = v4; break; }
        if (!v4->ip && v4->mode == IPV4_CFG_DHCP && !dhcp) dhcp = v4;
    }
    if (!chosen) chosen = dhcp;

    if (out_ifx) *out_ifx = l2->ifindex;
    if (out_src) *out_src = (chosen && chosen->ip) ? chosen->ip : 0;
    if (out_nh) *out_nh = 0xFFFFFFFFu;
    return true;
}

static bool pick_broadcast_global(uint8_t* out_ifx, uint32_t* out_src, uint32_t* out_nh) {
    l3_ipv4_interface_t* static_cand = NULL;
    l3_ipv4_interface_t* dhcp_cand = NULL;
    l2_interface_t* l2_s = NULL;
    l2_interface_t* l2_d = NULL;

    uint8_t n = l2_interface_count();
    for (uint8_t i = 0; i < n; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2) continue;
        for (int s = 0; s < MAX_IPV4_PER_INTERFACE; s++) {
            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
            if (!v4) continue;
            if (v4->mode == IPV4_CFG_DISABLED) continue;
            if (v4->ip && !static_cand) { static_cand = v4; l2_s = l2; }
            if (!v4->ip && v4->mode == IPV4_CFG_DHCP && !dhcp_cand) { dhcp_cand = v4; l2_d = l2; }
        }
    }

    l3_ipv4_interface_t* pick = static_cand ? static_cand : dhcp_cand;
    l2_interface_t* l2 = static_cand ? l2_s : l2_d;
    if (!l2) return false;

    if (out_ifx) *out_ifx = l2->ifindex;
    if (out_src) *out_src = (pick && pick->ip) ? pick->ip : 0;
    if (out_nh) *out_nh = 0xFFFFFFFFu;
    return true;
}

static bool pick_route_global(uint32_t dst, uint8_t* out_ifx, uint32_t* out_src, uint32_t* out_nh) {
    if (is_lbcast(dst)) return pick_broadcast_global(out_ifx, out_src, out_nh);

    ip_resolution_result_t r = resolve_ipv4_to_interface(dst);
    if (r.found && r.ipv4 && r.l2) {
        uint32_t m = r.ipv4->mask;
        if (m && ((dst & m) == (r.ipv4->ip & m))) {
            if (out_ifx) *out_ifx = r.l2->ifindex;
            if (out_src) *out_src = r.ipv4->ip;
            if (out_nh) *out_nh = dst;
            return true;
        }
        if (r.ipv4->gw) {
            if (out_ifx) *out_ifx = r.l2->ifindex;
            if (out_src) *out_src = r.ipv4->ip;
            if (out_nh) *out_nh = r.ipv4->gw;
            return true;
        }
        if (r.ipv4->routing_table) {
            uint32_t nh = 0; int pl = -1; int metric = 0;
            if (ipv4_rt_lookup_in(r.ipv4->routing_table, dst, &nh, &pl, &metric)) {
                if (out_ifx) *out_ifx = r.l2->ifindex;
                if (out_src) *out_src = r.ipv4->ip;
                if (out_nh) *out_nh = nh ? nh : dst;
                return true;
            }
        }
    }
    return lookup_route_in_tables(dst, out_nh, out_ifx, out_src);
}

static bool pick_route_bound_l3(uint8_t l3_id, uint32_t dst, uint8_t* out_ifx, uint32_t* out_src, uint32_t* out_nh) {
    if (is_lbcast(dst)) return pick_broadcast_bound_l3(l3_id, out_ifx, out_src, out_nh);

    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(l3_id);
    if (!v4 || !v4->l2) return false;
    if (v4->mode == IPV4_CFG_DISABLED) return false;
    if (!v4->ip) return false;

    uint32_t m = v4->mask;
    if (m && ((dst & m) == (v4->ip & m))) {
        if (out_ifx) *out_ifx = v4->l2->ifindex;
        if (out_src) *out_src = v4->ip;
        if (out_nh) *out_nh = dst;
        return true;
    }
    if (v4->gw) {
        if (out_ifx) *out_ifx = v4->l2->ifindex;
        if (out_src) *out_src = v4->ip;
        if (out_nh) *out_nh = v4->gw;
        return true;
    }
    if (v4->routing_table) {
        uint32_t nh = 0; int pl = -1; int metric = 0;
        if (ipv4_rt_lookup_in(v4->routing_table, dst, &nh, &pl, &metric)) {
            if (out_ifx) *out_ifx = v4->l2->ifindex;
            if (out_src) *out_src = v4->ip;
            if (out_nh) *out_nh = nh ? nh : dst;
            return true;
        }
    }
    return false;
}

static bool pick_route_bound_l2(uint8_t ifindex, uint32_t dst, uint8_t* out_ifx, uint32_t* out_src, uint32_t* out_nh) {
    if (is_lbcast(dst)) return pick_broadcast_bound_l2(ifindex, out_ifx, out_src, out_nh);

    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return false;

    l3_ipv4_interface_t* v4 = best_v4_on_l2_for_dst(l2, dst);
    if (!v4) return false;
    if (v4->mode == IPV4_CFG_DISABLED) return false;

    uint32_t m = v4->mask;
    if (m && ((dst & m) == (v4->ip & m))) {
        if (out_ifx) *out_ifx = l2->ifindex;
        if (out_src) *out_src = v4->ip;
        if (out_nh) *out_nh = dst;
        return true;
    }
    if (v4->gw) {
        if (out_ifx) *out_ifx = l2->ifindex;
        if (out_src) *out_src = v4->ip;
        if (out_nh) *out_nh = v4->gw;
        return true;
    }
    if (v4->routing_table) {
        uint32_t nh = 0; int pl = -1; int metric = 0;
        if (ipv4_rt_lookup_in(v4->routing_table, dst, &nh, &pl, &metric)) {
            if (out_ifx) *out_ifx = l2->ifindex;
            if (out_src) *out_src = v4->ip;
            if (out_nh) *out_nh = nh ? nh : dst;
            return true;
        }
    }
    return false;
}

static bool pick_route(uint32_t dst, const ipv4_tx_opts_t* opts, uint8_t* out_ifx, uint32_t* out_src, uint32_t* out_nh) {
    if (opts) {
        if (opts->scope == IPV4_TX_BOUND_L3) return pick_route_bound_l3(opts->index, dst, out_ifx, out_src, out_nh);
        if (opts->scope == IPV4_TX_BOUND_L2) return pick_route_bound_l2(opts->index, dst, out_ifx, out_src, out_nh);
        return pick_route_global(dst, out_ifx, out_src, out_nh);
    }
    return pick_route_global(dst, out_ifx, out_src, out_nh);
}


void ipv4_send_packet(uint32_t dst_ip, uint8_t proto, sizedptr segment, const ipv4_tx_opts_t* opts, uint8_t ttl) {
    if (!segment.ptr || !segment.size) return;

    uint8_t ifx = 0;
    uint32_t src_ip = 0;
    uint32_t nh = 0;
    if (!pick_route(dst_ip, opts, &ifx, &src_ip, &nh)) return;

    uint8_t dst_mac[6];
    bool is_dbcast = false;
    l2_interface_t* l2 = l2_interface_find_by_index(ifx);
    if (l2) {
        for (int s = 0; s < MAX_IPV4_PER_INTERFACE; ++s) {
            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
            if (!v4 || v4->mode == IPV4_CFG_DISABLED) continue;
            if (is_directed_bcast_for(v4, dst_ip)) { is_dbcast = true; break; }
        }
    }

    if (is_dbcast) {
        memset(dst_mac, 0xFF, 6);
    } else {
        if (!arp_resolve_on(ifx, nh, dst_mac, 200)) return;
    }

    uint32_t hdr_len = IP_IHL_NOOPTS * 4;
    uint32_t total = hdr_len + (uint32_t)segment.size;

    uintptr_t buf = (uintptr_t)malloc(total);
    if (!buf) return;

    ipv4_hdr_t* ip = (ipv4_hdr_t*)buf;
    ip->version_ihl = (uint8_t)((IP_VERSION_4 << 4) | IP_IHL_NOOPTS);
    ip->dscp_ecn = 0;
    ip->total_length = bswap16((uint16_t)total);
    ip->identification = bswap16(g_ip_ident++);
    ip->flags_frag_offset = bswap16(0);
    ip->ttl = ttl ? ttl : IP_TTL_DEFAULT;
    ip->protocol = proto;
    ip->header_checksum = 0;
    ip->src_ip = bswap32(src_ip);
    ip->dst_ip = bswap32(dst_ip);

    memcpy((void*)(buf + hdr_len), (const void*)segment.ptr, segment.size);
    ip->header_checksum = checksum16((const uint16_t*)ip, hdr_len / 2);

    sizedptr payload = { buf, total };
    eth_send_frame_on(ifx, ETHERTYPE_IPV4, dst_mac, payload);

    free((void*)buf, total);
}

void ipv4_input(uint16_t ifindex, uintptr_t ip_ptr, uint32_t ip_len, const uint8_t src_mac[6]) {
    if (ip_len < sizeof(ipv4_hdr_t)) return;

    ipv4_hdr_t* ip = (ipv4_hdr_t*)ip_ptr;
    uint8_t ver = (uint8_t)(ip->version_ihl >> 4);
    uint8_t ihl = (uint8_t)(ip->version_ihl & 0x0F);
    if (ver != IP_VERSION_4) return;
    if (ihl < IP_IHL_NOOPTS) return;

    uint32_t hdr_len = (uint32_t)ihl * 4;
    if (ip_len < hdr_len) return;

    uint16_t saved = ip->header_checksum;
    ip->header_checksum = 0;
    if (checksum16((const uint16_t*)ip, hdr_len / 2) != saved) {
        ip->header_checksum = saved;
        return;
    }
    ip->header_checksum = saved;

    uint16_t ip_totlen = bswap16(ip->total_length);
    if (ip_totlen < hdr_len) return;
    if (ip_len < ip_totlen) return;

    uintptr_t l4 = ip_ptr + hdr_len;
    uint32_t l4_len = (uint32_t)ip_totlen - hdr_len;

    uint32_t src = bswap32(ip->src_ip);
    uint32_t dst = bswap32(ip->dst_ip);

    if (ifindex && src) {
        uint8_t mac_old[6];
        bool had = arp_table_get_for_l2((uint8_t)ifindex, src, mac_old);
        if (!had || memcmp(mac_old, src_mac, 6) != 0) {
            arp_table_put_for_l2((uint8_t)ifindex, src, src_mac, 180000, false);
        } else {
            arp_table_put_for_l2((uint8_t)ifindex, src, mac_old, 180000, false);
        }
    }

    uint8_t proto = ip->protocol;

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

    if (is_mcast(dst)) return;

    if (is_lbcast(dst)) {
        if (ccount == 1) {
            uint8_t l3id = cand[0]->l3_id;
            switch (proto) {
                case 1: icmp_input(l4, l4_len, src, dst); break;
                case 6: tcp_input(IP_VER4, &src, &dst, l3id, l4, l4_len); break;
                case 17: udp_input(IP_VER4, &src, &dst, l3id, l4, l4_len); break;
                default: break;
            }
            return;
        } else {
            for (int i = 0; i < ccount; ++i) {
                uint8_t l3id = cand[i]->l3_id;
                switch (proto) {
                    case 1: icmp_input(l4, l4_len, src, dst); break;
                    case 6: tcp_input(IP_VER4, &src, &dst, l3id, l4, l4_len); break;
                    case 17: udp_input(IP_VER4, &src, &dst, l3id, l4, l4_len); break;
                    default: break;
                }
            }
            return;
        }
    }

    int match_count = 0;
    uint8_t match_l3id = 0;
    for (int i = 0; i < ccount; ++i) {
        if (cand[i]->ip && cand[i]->ip == dst) {
            match_count++;
            match_l3id = cand[i]->l3_id;
        }
    }
    if (match_count == 1) {
        switch (proto) {
            case 1: icmp_input(l4, l4_len, src, dst); break;
            case 6: tcp_input(IP_VER4, &src, &dst, match_l3id, l4, l4_len); break;
            case 17: udp_input(IP_VER4, &src, &dst, match_l3id, l4, l4_len); break;
            default: break;
        }
        return;
    }
    if (match_count > 1) {
        for (int i = 0; i < ccount; ++i) {
            if (cand[i]->ip == dst) {
                uint8_t l3id = cand[i]->l3_id;
                switch (proto) {
                    case 1: icmp_input(l4, l4_len, src, dst); break;
                    case 6: tcp_input(IP_VER4, &src, &dst, l3id, l4, l4_len); break;
                    case 17: udp_input(IP_VER4, &src, &dst, l3id, l4, l4_len); break;
                    default: break;
                }
            }
        }
        return;
    }

    int any_dbcast = 0;
    for (uint8_t i = 0, n = l2_interface_count(); i < n; ++i) {
        l2_interface_t* l2x = l2_interface_at(i);
        if (!l2x) continue;
        for (int s = 0; s < MAX_IPV4_PER_INTERFACE; ++s) {
            l3_ipv4_interface_t* v4 = l2x->l3_v4[s];
            if (!v4) continue;
            if (v4->mode == IPV4_CFG_DISABLED) continue;
            if (is_directed_bcast_for(v4, dst)) {
                any_dbcast = 1;
                uint8_t l3id = v4->l3_id;
                switch (proto) {
                    case 1: icmp_input(l4, l4_len, src, dst); break;
                    case 6: tcp_input(IP_VER4, &src, &dst, l3id, l4, l4_len); break;
                    case 17: udp_input(IP_VER4, &src, &dst, l3id, l4, l4_len); break;
                    default: break;
                }
            }
        }
    }
    if (any_dbcast) return;

    return;
}