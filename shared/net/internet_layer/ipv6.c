#include "ipv6.h"
#include "ipv6_utils.h"
#include "std/memory.h"
#include "std/string.h"
#include "net/link_layer/eth.h"
#include "networking/interface_manager.h"
#include "net/link_layer/ndp.h"
#include "net/transport_layer/tcp.h"
#include "net/transport_layer/udp.h"
#include "net/network_types.h"
#include "console/kio.h"
#include "syscalls/syscalls.h"
#include "net/internet_layer/ipv6_route.h"
#include "net/internet_layer/icmpv6.h"

static l3_ipv6_interface_t* best_v6_on_l2_for_dst(l2_interface_t* l2, const uint8_t dst[16]) {
    l3_ipv6_interface_t* best = NULL;
    int best_cmp = -1;
    int best_cost = 0x7FFFFFFF;
    int dst_is_ll = (ipv6_is_linklocal(dst) || ipv6_is_linkscope_mcast(dst)) ? 1 : 0;

    for (int s = 0; s < MAX_IPV6_PER_INTERFACE; s++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[s];
        if (!v6) continue;
        if (v6->cfg == IPV6_CFG_DISABLE) continue;
        if (ipv6_is_unspecified(v6->ip)) continue;
        if (v6->dad_state != IPV6_DAD_OK) continue;

        int v6_is_ll = ipv6_is_linklocal(v6->ip) ? 1 : 0;
        if (v6_is_ll != dst_is_ll) continue;

        int cmp = ipv6_common_prefix_len(dst, v6->ip);
        int cost = (int)l2->base_metric;
        if (cmp > best_cmp || (cmp == best_cmp && cost < best_cost)) {
            best_cmp = cmp;
            best_cost = cost;
            best = v6;
        }
    }
    return best;
}

static bool pick_route_bound_l3(uint8_t l3_id, const uint8_t dst[16], uint8_t* out_ifx, uint8_t out_src[16], uint8_t out_nh[16]) {
    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(l3_id);
    if (!v6 || !v6->l2) return false;
    if (v6->cfg == IPV6_CFG_DISABLE) return false;
    if (ipv6_is_unspecified(v6->ip)) return false;
    if (v6->dad_state != IPV6_DAD_OK) return false;

    int dst_is_ll = (ipv6_is_linklocal(dst) || ipv6_is_linkscope_mcast(dst)) ? 1 : 0;
    int src_is_ll = ipv6_is_linklocal(v6->ip) ? 1 : 0;
    if (dst_is_ll != src_is_ll) return false;

    if (out_ifx) *out_ifx = v6->l2->ifindex;
    if (out_src) ipv6_cpy(out_src, v6->ip);

    uint8_t nh[16];
    ipv6_cpy(nh, dst);

    if (!dst_is_ll && v6->prefix_len && ipv6_common_prefix_len(dst, v6->ip) < v6->prefix_len) {
        uint8_t via[16] = {0};
        int pl = -1,met = 0x7FFF;

        if (v6->routing_table &&
            ipv6_rt_lookup_in((const ipv6_rt_table_t*)v6->routing_table, dst, via, &pl, &met))
        {
            if (!ipv6_is_unspecified(via)) ipv6_cpy(nh, via);
        } else if (!ipv6_is_unspecified(v6->gateway) && ipv6_is_linklocal(v6->gateway)) {
            ipv6_cpy(nh, v6->gateway);
        }
    }

    if (out_nh) ipv6_cpy(out_nh, nh);
    return true;
}

static bool pick_route_bound_l2(uint8_t ifindex, const uint8_t dst[16], uint8_t* out_ifx, uint8_t out_src[16], uint8_t out_nh[16]) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return false;

    l3_ipv6_interface_t* v6 = best_v6_on_l2_for_dst(l2, dst);
    if (!v6) return false;

    if (out_ifx) *out_ifx = l2->ifindex;
    if (out_src) ipv6_cpy(out_src, v6->ip);

    uint8_t nh[16];
    ipv6_cpy(nh, dst);

    if (!ipv6_is_linklocal(dst) && v6->prefix_len && ipv6_common_prefix_len(dst, v6->ip) < v6->prefix_len) {
        uint8_t via[16] = {0};
        int pl = -1;
        int met =0x7FFF;

        if (v6->routing_table && ipv6_rt_lookup_in((const ipv6_rt_table_t*)v6->routing_table, dst, via, &pl, &met)) {
            if (!ipv6_is_unspecified(via)) ipv6_cpy(nh, via);
        } else if (!ipv6_is_unspecified(v6->gateway) && ipv6_is_linklocal(v6->gateway))ipv6_cpy(nh, v6->gateway);
    }

    if (out_nh) ipv6_cpy(out_nh, nh);
    return true;
}

static bool pick_route_global(const uint8_t dst[16], uint8_t* out_ifx, uint8_t out_src[16], uint8_t out_nh[16]) {
    int dst_is_ll = ipv6_is_linklocal(dst) ? 1 : 0;

    ip_resolution_result_t r = resolve_ipv6_to_interface(dst);
    if (r.found && r.ipv6 && r.l2) {
        if (r.ipv6->cfg != IPV6_CFG_DISABLE &&
            !ipv6_is_unspecified(r.ipv6->ip) &&
            r.ipv6->dad_state == IPV6_DAD_OK)
        {
            int src_is_ll = ipv6_is_linklocal(r.ipv6->ip) ? 1 : 0;
            if (src_is_ll == dst_is_ll) {
                if (out_ifx) *out_ifx = r.l2->ifindex;
                if (out_src) ipv6_cpy(out_src, r.ipv6->ip);

                uint8_t nh[16];
                ipv6_cpy(nh, dst);

                if (!dst_is_ll && r.ipv6->prefix_len && ipv6_common_prefix_len(dst, r.ipv6->ip) < r.ipv6->prefix_len) {
                    uint8_t via[16] = {0};
                    int pl = -1;
                    int met = 0x7FFF;

                    if (r.ipv6->routing_table && ipv6_rt_lookup_in((const ipv6_rt_table_t*)r.ipv6->routing_table, dst, via, &pl, &met)) {
                        if (!ipv6_is_unspecified(via)) ipv6_cpy(nh, via);
                    } else if (!ipv6_is_unspecified(r.ipv6->gateway) && ipv6_is_linklocal(r.ipv6->gateway)) {
                        ipv6_cpy(nh, r.ipv6->gateway);
                    }
                }

                if (out_nh) ipv6_cpy(out_nh, nh);
                return true;
            }
        }
    }

    uint8_t best_ifx = 0;
    uint8_t best_src[16] ={0};
    uint8_t best_nh[16] ={0};
    int best_pl = -1;
    int best_cost = 0x7FFFFFFF;

    uint8_t n = l2_interface_count();
    for (uint8_t i = 0; i < n; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2) continue;

        for (int s = 0; s< MAX_IPV6_PER_INTERFACE; s++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[s];
            if (!v6) continue;
            if (v6->cfg == IPV6_CFG_DISABLE) continue;
            if (ipv6_is_unspecified(v6->ip)) continue;
            if (v6->dad_state != IPV6_DAD_OK) continue;

            int src_is_ll = ipv6_is_linklocal(v6->ip) ? 1 : 0;
            if (src_is_ll != dst_is_ll) continue;

            int pl_conn = -1;
            if (!dst_is_ll && v6->prefix_len && ipv6_common_prefix_len(dst, v6->ip) >= v6->prefix_len) pl_conn = v6->prefix_len;
            if (dst_is_ll) pl_conn = 128;

            int pl_tab = -1;
            int met_tab = 0x7FFF;
            uint8_t via[16] = {0};

            if (!dst_is_ll && v6->routing_table) {
                int out_pl = -1;
                int out_met = 0x7FFF;
                if(ipv6_rt_lookup_in((const ipv6_rt_table_t*)v6->routing_table, dst, via, &out_pl, &out_met)) {
                    pl_tab = out_pl;
                    met_tab = out_met;
                }
            }

            int cand_pl = pl_conn;
            int cand_cost = (int)l2->base_metric;
            uint8_t cand_nh[16];
            ipv6_cpy(cand_nh, dst);

            if (pl_tab > cand_pl || (pl_tab == cand_pl && ((int)l2->base_metric + met_tab) < cand_cost)) {
                cand_pl =pl_tab;
                cand_cost = (int)l2->base_metric + met_tab;
                if (!ipv6_is_unspecified(via)) ipv6_cpy(cand_nh, via);
            } else if (cand_pl < 0) {
                if (!ipv6_is_unspecified(v6->gateway) && ipv6_is_linklocal(v6->gateway)) ipv6_cpy(cand_nh, v6->gateway);
            }

            if (cand_pl > best_pl || (cand_pl == best_pl && cand_cost < best_cost)) {
                best_pl = cand_pl;
                best_cost = cand_cost;
                best_ifx = l2->ifindex;
                ipv6_cpy(best_src, v6->ip);
                ipv6_cpy(best_nh, cand_nh);
            }
        }
    }

    if (best_pl < 0) return false;
    if (out_ifx) *out_ifx = best_ifx;
    if (out_src) ipv6_cpy(out_src, best_src);
    if (out_nh) ipv6_cpy(out_nh, best_nh);
    return true;
}

static bool pick_route(const uint8_t dst[16], const ipv6_tx_opts_t* opts, uint8_t* out_ifx, uint8_t out_src[16], uint8_t out_nh[16]) {
    if (opts) {
        if (opts->scope == IP_TX_BOUND_L3) return pick_route_bound_l3(opts->index, dst, out_ifx, out_src, out_nh);
        if (opts->scope == IP_TX_BOUND_L2) return pick_route_bound_l2(opts->index, dst, out_ifx, out_src, out_nh);
    }
    return pick_route_global(dst, out_ifx, out_src, out_nh);
}

void ipv6_send_packet(const uint8_t dst[16], uint8_t next_header, sizedptr segment, const ipv6_tx_opts_t* opts, uint8_t hop_limit) {
    if (!dst || !segment.ptr || !segment.size) return;

    uint8_t ifx = 0;
    uint8_t src[16] = {0};
    uint8_t nh[16] = {0};

    if (!pick_route(dst, opts, &ifx, src, nh)) return;

    if (!ipv6_is_unspecified(src)) {
        l2_interface_t* l2 = l2_interface_find_by_index(ifx);
        if (!l2) return;

        int ok = 0;
        for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[i];
            if (!v6) continue;
            if (ipv6_cmp(v6->ip, src) != 0) continue;
            if (v6->cfg == IPV6_CFG_DISABLE) return;
            if (v6->dad_state != IPV6_DAD_OK) return;
            ok = 1;
            break;
        }
        if (!ok) return;
    }

    if (ipv6_is_linklocal(src) && !ipv6_is_linklocal(dst) && !ipv6_is_multicast(dst)) return;

    uint8_t dst_mac[6];
    if (ipv6_is_multicast(dst)) ipv6_multicast_mac(dst, dst_mac);
    else if (!ndp_resolve_on(ifx, nh, dst_mac, 200)) return;

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

    if (ipv6_is_linklocal(ip6->src) &&
    !ipv6_is_linklocal(ip6->dst) &&
    !ipv6_is_multicast(ip6->dst) &&
    ip6->next_header != 58){
        bool dst_is_local = false;

        l2_interface_t* l2 = l2_interface_find_by_index((uint8_t)ifindex);
        if (l2) {
            for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
                l3_ipv6_interface_t* v6 = l2->l3_v6[i];
                if (!v6) continue;
                if (v6->cfg == IPV6_CFG_DISABLE) continue;
                if (ipv6_cmp(v6->ip, ip6->dst) == 0) {
                    dst_is_local = true;
                    break;
                }
            }
        }

        if (!dst_is_local) return;
    }

    uintptr_t l4 = ip_ptr + sizeof(ipv6_hdr_t);
    uint32_t l4_len = (uint32_t)payload_len;

    if (ipv6_is_linklocal(ip6->dst) && !ipv6_is_unspecified(ip6->src) && !ipv6_is_linklocal(ip6->src)) return;

    if (ifindex && !ipv6_is_unspecified(ip6->src) && src_mac) {
        ndp_table_put_for_l2((uint8_t)ifindex, ip6->src, src_mac, 180000, false);
    }

    if (ip6->next_header == 58) {
        icmpv6_input(ifindex, ip6->src, ip6->dst, ip6->hop_limit, src_mac, (const uint8_t*)l4, l4_len);
        return;
    }

    l2_interface_t* l2 = l2_interface_find_by_index((uint8_t)ifindex);
    if (!l2) return;

    l3_ipv6_interface_t* cand[MAX_IPV6_PER_INTERFACE];
    int ccount = 0;
    for (int s = 0; s < MAX_IPV6_PER_INTERFACE; ++s) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[s];
        if (!v6) continue;
        if (v6->cfg == IPV6_CFG_DISABLE) continue;
        cand[ccount++] = v6;
    }
    if (ccount == 0) return;

    if (ipv6_is_multicast(ip6->dst)) {

        int joined = 0;
        for (int m = 0; m < l2->ipv6_mcast_count; m++) {
            if (ipv6_cmp(l2->ipv6_mcast[m], ip6->dst) == 0) {
                joined = 1;
                break;
            }
        }
        if (!joined) return;

        for (int i = 0; i < ccount; i++) {
            l3_ipv6_interface_t* v6 = cand[i];
            if (!ipv6_is_linklocal(v6->ip) && ipv6_is_linklocal(ip6->dst))
                continue;

            switch (ip6->next_header) {
            case 17:
                udp_input(IP_VER6, ip6->src, ip6->dst, v6->l3_id, l4, l4_len);
                break;
            case 6:
                tcp_input(IP_VER6, ip6->src, ip6->dst, v6->l3_id, l4, l4_len);
                break;
            default:
                break;
            }
        }
        return;
    }

    int match_count = 0;
    uint8_t match_l3id = 0;
    for (int i = 0; i < ccount; ++i) {
        if (ipv6_cmp(cand[i]->ip, ip6->dst) == 0) {
            match_count++;
            if (match_count == 1) match_l3id = cand[i]->l3_id;
        }
    }

    if (match_count >= 1) {
        switch (ip6->next_header) {
        case 6:
            tcp_input(IP_VER6, ip6->src, ip6->dst, match_l3id, l4, l4_len);
            break;
        case 17:
            udp_input(IP_VER6, ip6->src, ip6->dst, match_l3id, l4, l4_len);
            break;
        default:
            break;
        }
        return;
    }
}