#include "ipv6.h"
#include "ipv6_utils.h"
#include "std/memory.h"
#include "std/string.h"
#include "networking/link_layer/eth.h"
#include "networking/interface_manager.h"
#include "networking/link_layer/ndp.h"
#include "networking/transport_layer/tcp.h"
#include "networking/transport_layer/udp.h"
#include "net/network_types.h"
#include "console/kio.h"
#include "syscalls/syscalls.h"
#include "networking/internet_layer/ipv6_route.h"
#include "networking/internet_layer/icmpv6.h"
#include "math/rng.h"
#include "net/checksums.h"
#include "networking/link_layer/nic_types.h"

#define IPV6_MIN_MTU 1280u
#define PMTU_CACHE_SIZE 16
#define REASS_SLOTS 8

typedef struct {
    uint8_t used;
    uint8_t dst[16];
    uint16_t mtu;
    uint32_t timestamp_ms;
} pmtu_entry_t;

typedef struct {
    uint8_t used;
    uint8_t ifindex;
    uint32_t ident;
    uint8_t src[16];
    uint8_t dst[16];
    uint8_t next_header;

    uint32_t first_rx_ms;
    uint32_t last_update_ms;

    uint32_t total_len;
    uint8_t have_last;
    uint8_t have_first;
    uint8_t first_src_mac[6];
    uint8_t _pad0[1];

    uint16_t first_pkt_len;
    uint8_t _pad1[2];
    uint8_t first_pkt[1280];

    uint8_t *buf;
    uint8_t bitmap[2048];
} reass_slot_t;

typedef struct __attribute__((packed)) {
    uint8_t next_header;
    uint8_t reserved;
    uint16_t offset_flags;
    uint32_t identification;
} ipv6_frag_hdr_t;

static pmtu_entry_t g_pmtu[PMTU_CACHE_SIZE] = {0};
static reass_slot_t g_reass[REASS_SLOTS] = {0};

uint16_t ipv6_pmtu_get(const uint8_t dst[16]) {
    if (!dst) return 0;
    uint32_t now = (uint32_t)get_time();
    for (int i = 0; i < PMTU_CACHE_SIZE; i++) {
        if (!g_pmtu[i].used) continue;
        if (ipv6_cmp(g_pmtu[i].dst, dst) != 0) continue;
        if (now - g_pmtu[i].timestamp_ms > 600000u) {
            g_pmtu[i].used = 0;
            continue;
        }
        return g_pmtu[i].mtu;
    }
    return 0;
}

void ipv6_pmtu_note(const uint8_t dst[16], uint16_t mtu) {
    if (!dst) return;
    if (mtu < IPV6_MIN_MTU) mtu = IPV6_MIN_MTU;
    uint32_t now = (uint32_t)get_time();

    int free_i = -1;
    int lru_i = 0;
    uint32_t lru_t = 0xFFFFFFFFu;

    for (int i = 0; i < PMTU_CACHE_SIZE; i++) {
        if (g_pmtu[i].used) {
            if (ipv6_cmp(g_pmtu[i].dst, dst) == 0) {
                g_pmtu[i].mtu = mtu;
                g_pmtu[i].timestamp_ms = now;
                return;
            }
            if (g_pmtu[i].timestamp_ms < lru_t) {
                lru_t = g_pmtu[i].timestamp_ms;
                lru_i = i;
            }
        } else if (free_i < 0) free_i = i;
    }

    int idx = (free_i >= 0) ? free_i : lru_i;
    g_pmtu[idx].used = 1;
    ipv6_cpy(g_pmtu[idx].dst, dst);
    g_pmtu[idx].mtu = mtu;
    g_pmtu[idx].timestamp_ms = now;
}

static void reass_free(reass_slot_t *s) {
    if (!s) return;
    if (s->buf) free_sized(s->buf, 2048u * 8u);
    memset(s, 0, sizeof(*s));
}

static void icmpv6_send_error(uint8_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], const uint8_t dst_mac[6], uint8_t type, uint8_t code, uint32_t param32, const uint8_t *invoking, uint32_t invoking_len) {
    if (!ifindex || !src_ip || !dst_ip || !dst_mac || !invoking || !invoking_len) return;

    uint32_t max_invoke = 1280u;
    uint32_t base = (uint32_t)sizeof(icmpv6_hdr_t) + 4u;
    if (base >= max_invoke) return;

    uint32_t copy = invoking_len;
    if (copy > max_invoke - base) copy = max_invoke - base;

    uint32_t icmp_len = base + copy;
    uint8_t *buf = (uint8_t*)malloc(icmp_len);
    if (!buf) return;

    icmpv6_hdr_t *h = (icmpv6_hdr_t*)buf;
    h->type = type;
    h->code = code;
    h->checksum = 0;

    *(uint32_t*)(buf + sizeof(icmpv6_hdr_t)) = bswap32(param32);

    memcpy(buf + base, invoking, copy);

    h->checksum =bswap16(checksum16_pipv6(src_ip, dst_ip, 58, buf, icmp_len));

    icmpv6_send_on_l2(ifindex, dst_ip, src_ip, dst_mac, buf, icmp_len, 64);

    free_sized(buf, icmp_len);
}

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

void ipv6_send_packet(const uint8_t dst[16], uint8_t next_header, netpkt_t* pkt, const ipv6_tx_opts_t* opts, uint8_t hop_limit, uint8_t dontfrag) {
    if (!dst || !pkt || !netpkt_len(pkt)) {
        if (pkt) netpkt_unref(pkt);
        return;
    }

    uint8_t ifx = 0;
    uint8_t src[16] = {0};
    uint8_t nh[16] = {0};

    l3_ipv6_interface_t* src_v6 = NULL;

    if (!pick_route(dst, opts, &ifx, src, nh)) {
        netpkt_unref(pkt);
        return;
    }

    if (!ipv6_is_unspecified(src)) {
        l2_interface_t* l2 = l2_interface_find_by_index(ifx);
        if (!l2) {
            netpkt_unref(pkt);
            return;
        }

        int ok = 0;
        for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[i];
            if (!v6) continue;
            if (ipv6_cmp(v6->ip, src) != 0) continue;
            if (v6->cfg == IPV6_CFG_DISABLE) {
                netpkt_unref(pkt);
                return;
            }
            if (v6->dad_state != IPV6_DAD_OK) {
                netpkt_unref(pkt);
                return;
            }
            ok = 1;
            src_v6 = v6;
            break;
        }
        if (!ok) {
            netpkt_unref(pkt);
            return;
        }
    }

    if (ipv6_is_linklocal(src) && !ipv6_is_linklocal(dst) && !ipv6_is_multicast(dst)) {
        netpkt_unref(pkt);
        return;
    }

    uint8_t dst_mac[6];
    l2_interface_t* l2 = l2_interface_find_by_index(ifx);
    if (ipv6_is_multicast(dst)) ipv6_multicast_mac(dst, dst_mac);
    else if (l2 && l2->kind == NET_IFK_LOCALHOST) memset(dst_mac, 0, 6);
    else if (!ndp_resolve_on(ifx, nh, dst_mac, 200)) {
        netpkt_unref(pkt);
        return;
    }

    uint16_t mtu = 1500;

    if (!src_v6 && opts && opts->scope == IP_TX_BOUND_L3) src_v6 = l3_ipv6_find_by_id(opts->index);
    if (src_v6 && src_v6->mtu) mtu = src_v6->mtu;

    uint16_t pmtu = ipv6_pmtu_get(dst);
    if (pmtu && pmtu  <mtu) mtu = pmtu;

    if (mtu < IPV6_MIN_MTU) mtu = IPV6_MIN_MTU;
    uint32_t hdr_len = (uint32_t)sizeof(ipv6_hdr_t);
    uint32_t seg_len = netpkt_len(pkt);
    uint32_t total_l3 = hdr_len + seg_len;

    if (total_l3 <= (uint32_t)mtu) {
        void* hdrp = netpkt_push(pkt, hdr_len);
        if (!hdrp) {
            netpkt_unref(pkt);
            return;
        }

        ipv6_hdr_t* ip6 = (ipv6_hdr_t*)hdrp;
        ip6->ver_tc_fl = bswap32((uint32_t)(6u << 28));
        ip6->payload_len = bswap16((uint16_t)seg_len);
        ip6->next_header = next_header;
        ip6->hop_limit = hop_limit ? hop_limit : 64;
        memcpy(ip6->src, src, 16);
        memcpy(ip6->dst, dst, 16);

        eth_send_frame_on(ifx, ETHERTYPE_IPV6, dst_mac, pkt);
        return;
    }

    if (dontfrag) {
        netpkt_unref(pkt);
        return;
    }

    uint32_t frag_hdr_len = (uint32_t)sizeof(ipv6_frag_hdr_t);
    if ((uint32_t)mtu < hdr_len + frag_hdr_len + 8u) {
        netpkt_unref(pkt);
        return;
    }

    uint32_t max_chunk = (uint32_t)mtu - hdr_len - frag_hdr_len;
    max_chunk = (max_chunk / 8u) * 8u;
    if (max_chunk == 0) {
        netpkt_unref(pkt);
        return;
    }

    rng_t rng;
    uint64_t virt_timer;
    asm volatile ("mrs %0, cntvct_el0" : "=r"(virt_timer));
    rng_seed(&rng, virt_timer);
    uint32_t ident = rng_next32(&rng);

    uint32_t off = 0;
    const uint8_t* data = (const uint8_t*)netpkt_data(pkt);
    uint32_t data_len = seg_len;

    while (off < data_len) {
        uint32_t remain = data_len - off;
        uint32_t chunk = (remain > max_chunk)? max_chunk : remain;
        uint8_t more = (off + chunk < data_len) ? 1u : 0u;

        uint32_t payload_len = frag_hdr_len + chunk;
        uint32_t frame_len = hdr_len + payload_len;

        netpkt_t* fpkt = netpkt_alloc(frame_len, (uint32_t)sizeof(eth_hdr_t), 0);
        if (!fpkt) break;
        void* buf = netpkt_put(fpkt, frame_len);
        if (!buf) {
            netpkt_unref(fpkt);
            break;
        }

        ipv6_hdr_t* ip6 = (ipv6_hdr_t*)buf;
        ip6->ver_tc_fl = bswap32((uint32_t)(6u << 28));
        ip6->payload_len = bswap16((uint16_t)payload_len);
        ip6->next_header = 44;
        ip6->hop_limit = hop_limit ? hop_limit : 64;
        memcpy(ip6->src, src, 16);
        memcpy(ip6->dst, dst, 16);

        ipv6_frag_hdr_t* fh = (ipv6_frag_hdr_t*)((uintptr_t)buf + hdr_len);
        fh->next_header = next_header;
        fh->reserved = 0;
        uint16_t off_flags = (uint16_t)(((off / 8u) & 0x1FFFu) << 3);
        if (more) off_flags |= 0x0001u;
        fh->offset_flags = bswap16(off_flags);
        fh->identification = bswap32(ident);

        memcpy((uint8_t*)(fh + 1), data + off, chunk);

        eth_send_frame_on(ifx, ETHERTYPE_IPV6, dst_mac, fpkt);

        off += chunk;
    }

    netpkt_unref(pkt);
}

static bool ipv6_skip_ext_headers(uint8_t* nh, uintptr_t* l4, uint32_t* l4_len) {
    if (!nh || !l4 || !l4_len) return false;

    for(;;) {
        uint8_t h = *nh;
        if (h == 44) return true;

        if (h == 0 ||h == 43 || h == 60) {
            if (*l4_len < 2) return false;
            const uint8_t* p = (const uint8_t*)(*l4);
            uint8_t next = p[0];
            uint8_t hlen = p[1];
            uint32_t bytes = (uint32_t)(hlen + 1u)*8;
            if (bytes > *l4_len) return false;
            *nh = next;
            *l4 += bytes;
            *l4_len -= bytes;
            continue;
        }

        if (h == 51) {
            if (*l4_len < 2) return false;
            const uint8_t* p = (const uint8_t*)(*l4);
            uint8_t next = p[0];
            uint8_t plen = p[1];
            uint32_t bytes = ((uint32_t)plen + 2u)*4;
            if (bytes > *l4_len) return false;
            *nh = next;
            *l4 += bytes;
            *l4_len -= bytes;
            continue;
        }

        return true;
    }
}

void ipv6_input(uint16_t ifindex, netpkt_t* pkt, const uint8_t src_mac[6]) {
    if (!pkt) return;
    uint32_t ip_len = netpkt_len(pkt);
    uintptr_t ip_ptr = netpkt_data(pkt);
    if (ip_len < sizeof(ipv6_hdr_t)) return;

    ipv6_hdr_t* ip6 = (ipv6_hdr_t*)ip_ptr;
    uint32_t v = bswap32(ip6->ver_tc_fl);
    if ((v >> 28) != 6) return;
    uint32_t now = (uint32_t)get_time();
    for (int i = 0; i < REASS_SLOTS; i++) {
        reass_slot_t *s = &g_reass[i];
        if (!s->used) continue;

        if (now - s->first_rx_ms < 60000u) continue;

        if (s->have_first && s->first_pkt_len) {
            icmpv6_send_error(s->ifindex, s->dst, s->src, s->first_src_mac, 3, 1, 0, s->first_pkt, s->first_pkt_len);
        }

        reass_free(s);
    }

    uint16_t payload_len = bswap16(ip6->payload_len);
    if ((uint32_t)payload_len + sizeof(ipv6_hdr_t) > ip_len) return;
    (void)netpkt_trim(pkt, (uint32_t)payload_len + (uint32_t)sizeof(ipv6_hdr_t));
    ip_len = netpkt_len(pkt);

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

    if (ifindex && !ipv6_is_unspecified(ip6->src) && src_mac) ndp_table_put_for_l2((uint8_t)ifindex, ip6->src, src_mac, 180000, false);

    uint8_t nh = ip6->next_header;

    if (!ipv6_skip_ext_headers(&nh, &l4, &l4_len)) return;

    if (nh == 44) {//b
        if (l4_len < sizeof(ipv6_frag_hdr_t)) return;

        const ipv6_frag_hdr_t* fh = (const ipv6_frag_hdr_t*)l4;
        uint8_t inner_nh = fh->next_header;
        uint16_t off_flags = bswap16(fh->offset_flags);
        uint32_t ident = bswap32(fh->identification);

        uint32_t off = ((uint32_t)(off_flags >> 3) & 0x1FFFu) * 8u;
        uint8_t more = (off_flags & 0x0001u) ? 1u : 0u;

        const uint8_t* frag = (const uint8_t*)(fh + 1);
        uint32_t frag_len = l4_len-(uint32_t)sizeof(ipv6_frag_hdr_t);

        if (more && (frag_len & 7u)) {
            uint8_t invoke_buf[sizeof(ipv6_hdr_t) + sizeof(ipv6_frag_hdr_t) + 8];
            uint32_t inv_len = (uint32_t)sizeof(ipv6_hdr_t) + l4_len;
            const uint8_t *inv =(const uint8_t*) ip6;
            if (inv_len > sizeof(invoke_buf)) {
                memcpy(invoke_buf, ip6, sizeof(ipv6_hdr_t));
                uint32_t cpy = l4_len;
                uint32_t max = (uint32_t)sizeof(ipv6_frag_hdr_t) + 8u;
                if (cpy > max) cpy = max;
                memcpy(invoke_buf + sizeof(ipv6_hdr_t), (void*)l4, cpy);
                inv = invoke_buf;
                inv_len = (uint32_t)sizeof(invoke_buf);
            }
            icmpv6_send_error((uint8_t)ifindex, ip6->dst, ip6->src, src_mac, 4, 0, 4u, inv, inv_len);
            return;
        }

        if (off + frag_len > 65535u) {
            uint8_t invoke_buf[sizeof(ipv6_hdr_t) + sizeof(ipv6_frag_hdr_t) + 8];
            uint32_t inv_len = (uint32_t)sizeof(ipv6_hdr_t) + l4_len;
            const uint8_t *inv = (const uint8_t*)ip6;
            if (inv_len > sizeof(invoke_buf)) {
                memcpy(invoke_buf, ip6, sizeof(ipv6_hdr_t));
                uint32_t cpy = l4_len;
                uint32_t max = (uint32_t)sizeof(ipv6_frag_hdr_t) + 8u;
                if (cpy > max) cpy = max;
                memcpy(invoke_buf + sizeof(ipv6_hdr_t), (void*)l4, cpy);
                inv = invoke_buf;
                inv_len = (uint32_t)sizeof(invoke_buf);
            }
            icmpv6_send_error((uint8_t)ifindex, ip6->dst, ip6->src, src_mac, 4, 0, 42u, inv, inv_len);
            return;
        }

        if (off + frag_len > 2048u * 8u) return;

        reass_slot_t* s = NULL;
        uint32_t now = (uint32_t)get_time();

        for (int i = 0; i < REASS_SLOTS; i++) {
            reass_slot_t *t = &g_reass[i];
            if (!t->used) continue;
            if (t->ifindex != (uint8_t)ifindex) continue;
            if (t->ident != ident) continue;
            if (t->next_header != inner_nh) continue;
            if (ipv6_cmp(t->src, ip6->src) != 0) continue;
            if (ipv6_cmp(t->dst, ip6->dst) != 0) continue;
            if (now - t->last_update_ms > 60000u) {
                if (t->buf) free_sized(t->buf, 2048u * 8u);
                memset(t, 0, sizeof(*t));
                continue;
            }
            s = t;
            break;
        }

        if (!s) {
            for (int i = 0; i < REASS_SLOTS; i++) {
                reass_slot_t *t = &g_reass[i];
                if (t->used) continue;

                t->buf = (uint8_t*)malloc(2048u * 8u);
                if (!t->buf) return;

                t->used = 1;
                t->ifindex = (uint8_t)ifindex;
                t->ident = ident;
                ipv6_cpy(t->src, ip6->src);
                ipv6_cpy(t->dst, ip6->dst);
                t->next_header = inner_nh;
                t->first_rx_ms = now;
                t->last_update_ms = now;
                t->total_len = 0;
                t->have_last = 0;
                t->have_first = 0;
                memset(t->first_src_mac, 0, 6);
                t->first_pkt_len = 0;
                memset(t->first_pkt, 0, sizeof(t->first_pkt));
                memset(t->bitmap, 0, sizeof(t->bitmap));
                s = t;
                break;
            }
        }

        if (!s) return;
        int overlap = 0;
        uint32_t start = off / 8u;
        uint32_t end = (off + frag_len + 7u) / 8u;
        if (end > sizeof(s->bitmap)) end = sizeof(s->bitmap);
        for (uint32_t i = start; i < end; i++) {
            if (s->bitmap[i]) {
                overlap = 1;
                break;
            }
        }
        if (overlap) {
            reass_free(s);
            return;
        }

        int has_ulh = 0;
        if (off == 0) {
            uint32_t ulh_off = 0;
            uint8_t nh = inner_nh;
            int ok = 1;

            while (nh == 0 || nh == 43 || nh == 60 || nh == 51) {
                const uint8_t *p = frag + ulh_off;
                uint32_t avail = frag_len - ulh_off;
                if (avail < 2) { ok = 0; break; }

                uint32_t hlen = 0;
                if (nh == 0 || nh == 43 || nh == 60) hlen = ((uint32_t)p[1] + 1u) * 8u;
                else hlen = ((uint32_t)p[1] + 2u) * 4u;

                if (hlen > avail) { ok = 0; break; }

                nh = p[0];
                ulh_off += hlen;
            }

            if (ok) {
                uint32_t need = 1;
                if (nh == 6) need = 20;
                else if (nh == 17) need = 8;
                else if (nh == 58) need = 4;
                if (frag_len - ulh_off >= need) has_ulh = 1;
            }
        }

        if (off == 0 && !has_ulh) {
            uint8_t invoke_buf[sizeof(ipv6_hdr_t) + sizeof(ipv6_frag_hdr_t) + 64];
            uint32_t inv_len = (uint32_t)sizeof(ipv6_hdr_t) + l4_len;
            const uint8_t *inv = (const uint8_t*)ip6;
            if (inv_len > sizeof(invoke_buf)) {
                memcpy(invoke_buf, ip6, sizeof(ipv6_hdr_t));
                uint32_t cpy = l4_len;
                uint32_t max = (uint32_t)sizeof(ipv6_frag_hdr_t) + 64u;
                if (cpy > max) cpy = max;
                memcpy(invoke_buf + sizeof(ipv6_hdr_t), (void*)l4, cpy);
                inv = invoke_buf;
                inv_len = (uint32_t)sizeof(invoke_buf);
            }
            icmpv6_send_error((uint8_t)ifindex, ip6->dst, ip6->src, src_mac, 4, 3, 0u, inv, inv_len);
            reass_free(s);
            return;
        }

        if (off == 0 && !s->have_first) {
            uint32_t inv_len = (uint32_t)sizeof(ipv6_hdr_t) + l4_len;
            if (inv_len > sizeof(s->first_pkt)) inv_len = sizeof(s->first_pkt);
            memcpy(s->first_pkt, ip6, inv_len);
            s->first_pkt_len = (uint16_t)inv_len;
            memcpy(s->first_src_mac, src_mac, 6);
            s->have_first = 1;
        }

        memcpy(s->buf + off, frag, frag_len);

        start = off / 8u;
        end = (off + frag_len + 7u) / 8u;
        if (end > sizeof(s->bitmap)) end = sizeof(s->bitmap);
        for (uint32_t i = start; i < end; i++) s->bitmap[i] = 1;

        s->last_update_ms = (uint32_t)get_time();

        if (!more) {
            s->have_last = 1;
            s->total_len = off + frag_len;
        }

        int complete = 0;
        if (s->have_last) {
            uint32_t needed = (s->total_len + 7u) / 8u;
            if (needed <= sizeof(s->bitmap)) {
                complete = 1;
                for (uint32_t i = 0; i < needed; i++) if (!s->bitmap[i]) {
                    complete = 0;
                    break;
                }
            }
        }
        if (!complete) return;
        

        uintptr_t payload_ptr = (uintptr_t)s->buf;
        uint32_t payload_size = s->total_len;
        if (!ipv6_skip_ext_headers(&inner_nh, &payload_ptr, &payload_size)) {
            reass_free(s);
            return;
        }

        if (inner_nh == 58) {
            icmpv6_input(ifindex, ip6->src, ip6->dst, ip6->hop_limit, src_mac, (const uint8_t*)payload_ptr, payload_size);
            reass_free(s);
            return;
        }

        l2_interface_t* l2 = l2_interface_find_by_index((uint8_t)ifindex);
        if (!l2) { reass_free(s); return; }

        l3_ipv6_interface_t* cand[MAX_IPV6_PER_INTERFACE];
        int ccount = 0;
        for (int x = 0; x < MAX_IPV6_PER_INTERFACE; x++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[x];
            if (!v6) continue;
            if (v6->cfg == IPV6_CFG_DISABLE) continue;
            cand[ccount++] = v6;
        }
        if (ccount == 0) {
            reass_free(s);
            return;
        }

        if (ipv6_is_multicast(ip6->dst)) {
            int joined = 0;
            for (int m = 0; m < l2->ipv6_mcast_count; m++) {
                if (ipv6_cmp(l2->ipv6_mcast[m], ip6->dst) == 0) {
                    joined = 1;
                    break;
                }
            }
            if (!joined) {
                reass_free(s);
                return;
            }

            for (int i = 0; i < ccount; i++) {
                l3_ipv6_interface_t* v6 = cand[i];
                if (!ipv6_is_linklocal(v6->ip) && ipv6_is_linklocal(ip6->dst)) continue;
                if (inner_nh == 17) udp_input(IP_VER6, ip6->src, ip6->dst, v6->l3_id, payload_ptr, payload_size);
                else if (inner_nh == 6) tcp_input(IP_VER6, ip6->src, ip6->dst, v6->l3_id, payload_ptr, payload_size);
            }

            reass_free(s);
            return;
        }

        int match_count = 0;
        uint8_t match_l3id = 0;
        for (int i = 0; i < ccount; i++) {
            if (ipv6_cmp(cand[i]->ip, ip6->dst) == 0){
                match_count++;
                if (match_count == 1) match_l3id = cand[i]->l3_id;
            }
        }

        if (match_count >= 1) {
            if (inner_nh == 6) tcp_input(IP_VER6, ip6->src, ip6->dst, match_l3id, payload_ptr, payload_size);
            else if (inner_nh == 17) udp_input(IP_VER6, ip6->src, ip6->dst, match_l3id, payload_ptr, payload_size);
        }

        reass_free(s);
        return;
    }

    if (nh == 58) {
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