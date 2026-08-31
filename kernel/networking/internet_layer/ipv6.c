#include "ipv6.h"
#include "ipv6_utils.h"
#include "std/memory.h"
#include "std/string.h"
#include "networking/link_layer/eth.h"
#include "networking/link_layer/link_utils.h"
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
#include "random/random.h"
#include "net/checksums.h"
#include "networking/link_layer/nic_types.h"
#include "networking/net_fragbuf.h"
#include "networking/internet_layer/pmtu.h"

#define IPV6_MIN_MTU 1280u
#define REASS_SLOTS 8
typedef struct {
    uint8_t used;
    uint8_t ifindex;
    uint32_t ident;
    uint8_t src[16];
    uint8_t dst[16];
    uint8_t next_header;

    uint32_t first_rx_ms;
    uint32_t last_update_ms;

    uint8_t have_first;
    uint8_t first_src_mac[6];
    uint8_t _pad0[1];

    uint16_t first_pkt_len;
    uint8_t _pad1[2];
    uint8_t first_pkt[1280];
    net_fragbuf_t frag;
} reass_slot_t;

typedef struct __attribute__((packed)) {
    uint8_t next_header;
    uint8_t reserved;
    uint16_t offset_flags;
    uint32_t identification;
} ipv6_frag_hdr_t;

static reass_slot_t g_reass[REASS_SLOTS] = {0};

static void reass_free(reass_slot_t *s) {
    if (!s) return;
    net_fragbuf_free(&s->frag);
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
    uint8_t *buf = (uint8_t*)zalloc(icmp_len ? icmp_len : 1u);
    if (!buf) return;

    icmpv6_hdr_t *h = (icmpv6_hdr_t*)buf;
    h->type = type;
    h->code = code;
    h->checksum = 0;

    wr_be32(buf + sizeof(icmpv6_hdr_t), param32);

    memcpy(buf + base, invoking, copy);

    h->checksum =bswap16(checksum16_pipv6(src_ip, dst_ip, PROTO_ICMPV6, buf, icmp_len));

    icmpv6_send_on_l2(ifindex, dst_ip, src_ip, dst_mac, buf, icmp_len, 64);

    release(buf);
}

bool ipv6_send_packet(const uint8_t dst[16], uint8_t next_header, netpkt_t* pkt, const ip_tx_opts_t* opts, uint8_t hop_limit, uint8_t dontfrag) {
    if (!dst || !pkt || !netpkt_len(pkt)) {
        if (pkt) netpkt_unref(pkt);
        return false;
    }

    ipv6_tx_plan_t plan;
    if (!ipv6_build_tx_plan(dst, opts, &plan)) {
        netpkt_unref(pkt);
        return false;
    }

    l3_ipv6_interface_t* src_v6 = l3_ipv6_find_by_id(plan.l3_id);
    if (!ipv6_l3_is_ready(src_v6)) {
        netpkt_unref(pkt);
        return false;
    }

    uint8_t ifx = src_v6->l2->ifindex;
    uint8_t src[16];
    uint8_t nh[16];
    ipv6_cpy(src, plan.src_ip);
    ipv6_cpy(nh, dst);

    if (ipv6_is_linklocal(src) && !ipv6_is_linklocal(dst) && !ipv6_is_multicast(dst)) {
        netpkt_unref(pkt);
        return false;
    }

    if (!ipv6_is_linklocal(dst) && !ipv6_is_multicast(dst)) {
        uint8_t via[16] = {0};
        int route_pl = -1;
        bool have_nh = false;

        if (src_v6->routing_table && ipv6_rt_lookup_in((const ipv6_rt_table_t*)src_v6->routing_table, dst, via, &route_pl, NULL)) {
            if (!ipv6_is_unspecified(via)) ipv6_cpy(nh, via);
            else if (src_v6->prefix_len && ipv6_common_prefix_len(dst, src_v6->ip) >= src_v6->prefix_len) ipv6_cpy(nh, dst);
            else if (route_pl > 0) ipv6_cpy(nh, dst);
            else if (!ipv6_is_unspecified(src_v6->gateway) && ipv6_is_linklocal(src_v6->gateway)) ipv6_cpy(nh, src_v6->gateway);
            else {
                netpkt_unref(pkt);
                return false;
            }
            have_nh = true;
        }

        if (!have_nh && src_v6->prefix_len && ipv6_common_prefix_len(dst, src_v6->ip) >= src_v6->prefix_len) have_nh = true;
        if (!have_nh && !ipv6_is_unspecified(src_v6->gateway) && ipv6_is_linklocal(src_v6->gateway)) {
            ipv6_cpy(nh, src_v6->gateway);
            have_nh = true;
        }
        if (!have_nh) {
            netpkt_unref(pkt);
            return false;
        }
    }

    l2_interface_t* l2 = src_v6->l2;
    uint8_t dst_mac[6];
    bool need_ndp = false;
    if (ipv6_is_multicast(dst)) ipv6_multicast_mac(dst, dst_mac);
    else if (l2 && l2->kind == NET_IFK_LOCALHOST) mac_clear(dst_mac);
    else need_ndp = true;

    uint16_t mtu = l3_ipv6_effective_mtu(src_v6);
    if (mtu < IPV6_MIN_MTU) {
        netpkt_unref(pkt);
        return false;
    }
    if (next_header != PROTO_TCP) {
        uint16_t path_mtu = pmtu_get(src_v6->l3_id, src_v6->epoch, IP_VER6, dst);
        if (path_mtu && path_mtu < mtu) mtu = path_mtu;
    }
    if (mtu < IPV6_MIN_MTU) mtu = IPV6_MIN_MTU;
    uint32_t hdr_len = (uint32_t)sizeof(ipv6_hdr_t);
    uint32_t seg_len = netpkt_len(pkt);
    uint32_t total_l3 = hdr_len + seg_len;

    if (total_l3 <= (uint32_t)mtu) {
        void* hdrp = netpkt_push(pkt, hdr_len);
        if (!hdrp) {
            netpkt_unref(pkt);
            return false;
        }

        ipv6_hdr_t ip6;
        ip6.ver_tc_fl = bswap32((uint32_t)(6u << 28));
        ip6.payload_len = bswap16((uint16_t)seg_len);
        ip6.next_header = next_header;
        ip6.hop_limit = hop_limit ? hop_limit : (src_v6->l2->ipv6_default_hop_limit ? src_v6->l2->ipv6_default_hop_limit : 64);
        ipv6_cpy(ip6.src, src);
        ipv6_cpy(ip6.dst, dst);
        memcpy(hdrp, &ip6, sizeof(ip6));

        if (need_ndp) return ndp_send_or_queue_on(ifx, nh, pkt);
        return eth_send_frame_on(ifx, ETHERTYPE_IPV6, dst_mac, pkt);
    }

    uint32_t frag_hdr_len = sizeof(ipv6_frag_hdr_t);
    if (dontfrag || (uint32_t)mtu < hdr_len + frag_hdr_len + 8u) {
        netpkt_unref(pkt);
        return false;
    }

    uint32_t max_chunk = (uint32_t)mtu - hdr_len - frag_hdr_len;
    max_chunk = (max_chunk / 8u) * 8u;
    if (max_chunk == 0) {
        netpkt_unref(pkt);
        return false;
    }

    rng_t rng;
        rng_init_random(&rng);
    uint32_t ident = rng_next32(&rng);

    uint32_t off = 0;
    const uint8_t* data = (const uint8_t*)netpkt_data(pkt);
    bool ok = true;

    while (off < seg_len) {
        uint32_t remain = seg_len - off;
        uint32_t chunk = (remain > max_chunk)? max_chunk : remain;
        uint8_t more = off + chunk < seg_len ? 1u : 0;

        uint32_t payload_len = frag_hdr_len + chunk;
        uint32_t frame_len = hdr_len + payload_len;

        netpkt_t* fpkt = netpkt_alloc(frame_len, (uint32_t)sizeof(eth_hdr_t), 0);
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

        ipv6_hdr_t ip6;
        ip6.ver_tc_fl = bswap32((uint32_t)(6u << 28));
        ip6.payload_len = bswap16((uint16_t)payload_len);
        ip6.next_header = 44;
        ip6.hop_limit = hop_limit ? hop_limit : (src_v6->l2->ipv6_default_hop_limit ? src_v6->l2->ipv6_default_hop_limit : 64);
        memcpy(ip6.src, src, 16);
        memcpy(ip6.dst, dst, 16);

        memcpy(buf, &ip6, sizeof(ip6));
        ipv6_frag_hdr_t fh;
        fh.next_header = next_header;
        fh.reserved = 0;
        uint16_t off_flags = (uint16_t)(((off / 8u) & 0x1FFFu) << 3);
        if (more) off_flags |= 0x0001u;
        fh.offset_flags = bswap16(off_flags);
        fh.identification = bswap32(ident);
        memcpy((uint8_t*)buf + hdr_len, &fh, sizeof(fh));

        memcpy((uint8_t*)buf + hdr_len + sizeof(fh), data + off, chunk);

        if (need_ndp) { if (!ndp_send_or_queue_on(ifx, nh, fpkt)) ok = false; }
        else if (!eth_send_frame_on(ifx, ETHERTYPE_IPV6, dst_mac, fpkt)) ok = false;

        off += chunk;
    }

    netpkt_unref(pkt);
    return ok && off == seg_len;
}

static bool ipv6_skip_ext_headers(const netpkt_t* pkt, uint8_t* nh, uint32_t* l4_off, uint32_t* l4_len) {
    if (!pkt || !nh || !l4_off || !l4_len) return false;
    uint32_t total_len = netpkt_len(pkt);

    for(;;) {
        uint8_t ext[2];
        if (*l4_off > total_len || *l4_len > total_len - *l4_off) return false;
        uint8_t h = *nh;
        if (h == 44) return true;

        if (h == 0 || h == 43 || h == 60) {
            if (*l4_len < sizeof(ext)) return false;
            if (!netpkt_copyout(pkt, *l4_off, ext, sizeof(ext))) return false;
            uint32_t bytes = ((uint32_t)ext[1] + 1u)*8;
            if (bytes > *l4_len) return false;
            *nh = ext[0];
            *l4_off += bytes;
            *l4_len -= bytes;
            continue;
        }

        if (h == 51) {
            if (*l4_len < sizeof(ext)) return false;
            if (!netpkt_copyout(pkt, *l4_off, ext, sizeof(ext))) return false;
            uint32_t bytes = ((uint32_t)ext[1] + 2u)*4;
            if (bytes > *l4_len) return false;
            *nh = ext[0];
            *l4_off += bytes;
            *l4_len -= bytes;
            continue;
        }

        return true;
    }
}

void ipv6_input(uint8_t ifindex, netpkt_t* pkt, const uint8_t src_mac[6]) {
    if (!pkt) return;
    uint32_t ip_len = netpkt_len(pkt);
    if (ip_len < sizeof(ipv6_hdr_t)) return;

    ipv6_hdr_t ip6_;
    ipv6_hdr_t* ip6 = &ip6_;
    if (!netpkt_copyout(pkt, 0, ip6, sizeof(*ip6))) return;
    uint32_t v = bswap32(ip6->ver_tc_fl);
    if ((v >> 28) != 6) return;

    if (ipv6_is_loopback(ip6->src)) {
        l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
        if (!l2 || l2->kind != NET_IFK_LOCALHOST) return;
    }

    uint32_t now = (uint32_t)get_time();
    for (int i = 0; i < REASS_SLOTS; i++) {
        reass_slot_t *s = &g_reass[i];
        if (!s->used) continue;

        if (now - s->last_update_ms < 60000u) continue;

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
        ip6->next_header != PROTO_ICMPV6){
        bool dst_is_local = false;

        l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
        if (l2) {
            for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
                l3_ipv6_interface_t* v6 = l2->l3_v6[i];
                if (!ipv6_l3_is_active(v6)) continue;
                if (ipv6_cmp(v6->ip, ip6->dst) == 0) {
                    dst_is_local = true;
                    break;
                }
            }
        }

        if (!dst_is_local) return;
    }

    uint32_t l4_off = (uint32_t)sizeof(ipv6_hdr_t);
    uint32_t l4_len = (uint32_t)payload_len;

    if (ipv6_is_linklocal(ip6->dst) && !ipv6_is_unspecified(ip6->src) && !ipv6_is_linklocal(ip6->src)) return;

    uint8_t nh = ip6->next_header;

    if (!ipv6_skip_ext_headers(pkt, &nh, &l4_off, &l4_len)) return;

    if (nh == 44) {//b
        if (l4_len < sizeof(ipv6_frag_hdr_t)) return;

        ipv6_frag_hdr_t fh;
        if (!netpkt_copyout(pkt, l4_off, &fh, sizeof(fh))) return;
        uint8_t inner_nh = fh.next_header;
        uint16_t off_flags = bswap16(fh.offset_flags);
        uint32_t ident = bswap32(fh.identification);

        uint32_t off = ((uint32_t)(off_flags >> 3) & 0x1FFFu) * 8u;
        uint8_t more = (off_flags & 0x0001u) ? 1u : 0u;

        uint32_t frag_off = l4_off + (uint32_t)sizeof(ipv6_frag_hdr_t);
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
                if (!netpkt_copyout(pkt, l4_off, invoke_buf + sizeof(ipv6_hdr_t), cpy)) return;
                inv = invoke_buf;
                inv_len = (uint32_t)sizeof(invoke_buf);
            }
            icmpv6_send_error(ifindex, ip6->dst, ip6->src, src_mac, 4, 0, 4u, inv, inv_len);
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
                if (!netpkt_copyout(pkt, l4_off, invoke_buf + sizeof(ipv6_hdr_t), cpy)) return;
                inv = invoke_buf;
                inv_len = (uint32_t)sizeof(invoke_buf);
            }
            icmpv6_send_error(ifindex, ip6->dst, ip6->src, src_mac, 4, 0, 42u, inv, inv_len);
            return;
        }

        if (off + frag_len > NET_FRAGBUF_DEFAULT_MAX_LEN) return;

        reass_slot_t* s = NULL;
        uint32_t now = (uint32_t)get_time();

        for (int i = 0; i < REASS_SLOTS; i++) {
            reass_slot_t *t = &g_reass[i];
            if (!t->used) continue;
            if (t->ifindex != ifindex) continue;
            if (t->ident != ident) continue;
            if (t->next_header != inner_nh) continue;
            if (ipv6_cmp(t->src, ip6->src) != 0) continue;
            if (ipv6_cmp(t->dst, ip6->dst) != 0) continue;
            if (now - t->last_update_ms > 60000u) {
                reass_free(t);
                continue;
            }
            s = t;
            break;
        }

        if (!s) {
            for (int i = 0; i < REASS_SLOTS; i++) {
                reass_slot_t *t = &g_reass[i];
                if (t->used) continue;

                t->used = 1;
                t->ifindex = ifindex;
                t->ident = ident;
                ipv6_cpy(t->src, ip6->src);
                ipv6_cpy(t->dst, ip6->dst);
                t->next_header = inner_nh;
                t->first_rx_ms = now;
                t->last_update_ms = now;
                t->have_first = 0;
                net_fragbuf_init(&t->frag);
                mac_clear(t->first_src_mac);
                t->first_pkt_len = 0;
                memset(t->first_pkt, 0, sizeof(t->first_pkt));
                s = t;
                break;
            }
        }

        if (!s) return;

        int has_ulh = 0;
        if (off == 0) {
            uint32_t ulh_off = 0;
            uint8_t nh = inner_nh;
            int ok = 1;

            while (nh == 0 || nh == 43 || nh == 60 || nh == 51) {
                uint8_t ext[2];
                uint32_t avail = frag_len - ulh_off;
                if (avail < sizeof(ext)) { ok = 0; break; }
                if (!netpkt_copyout(pkt, frag_off + ulh_off, ext, sizeof(ext))) { ok = 0; break; }

                uint32_t hlen = 0;
                if (nh == 0 || nh == 43 || nh == 60) hlen = ((uint32_t)ext[1] + 1u) * 8u;
                else hlen = ((uint32_t)ext[1] + 2u) * 4u;

                if (hlen > avail) { ok = 0; break; }

                nh = ext[0];
                ulh_off += hlen;
            }

            if (ok) {
                uint32_t need = 1;
                if (nh == PROTO_TCP) need = 20;
                else if (nh == PROTO_UDP) need = 8;
                else if (nh == PROTO_ICMPV6) need = 4;
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
                if (!netpkt_copyout(pkt, l4_off, invoke_buf + sizeof(ipv6_hdr_t), cpy)) return;
                inv = invoke_buf;
                inv_len = (uint32_t)sizeof(invoke_buf);
            }
            icmpv6_send_error(ifindex, ip6->dst, ip6->src, src_mac, 4, 3, 0u, inv, inv_len);
            reass_free(s);
            return;
        }

        if (off == 0 && !s->have_first) {
            uint32_t inv_len = (uint32_t)sizeof(ipv6_hdr_t) + l4_len;
            if (inv_len > sizeof(s->first_pkt)) inv_len = sizeof(s->first_pkt);
            memcpy(s->first_pkt, ip6, sizeof(*ip6));
            if (inv_len > (uint32_t)sizeof(*ip6) && !netpkt_copyout(pkt, l4_off, s->first_pkt + sizeof(*ip6), inv_len - (uint32_t)sizeof(*ip6))) {
                reass_free(s);
                return;
            }
            s->first_pkt_len = (uint16_t)inv_len;
            mac_copy(s->first_src_mac, src_mac);
            s->have_first = 1;
        }

        if (!net_fragbuf_add(&s->frag, pkt, frag_off, off, frag_len, more)) {
            reass_free(s);
            return;
        }
        s->last_update_ms = now;

        if (!net_fragbuf_complete(&s->frag)) return;

        uint32_t payload_off = 0;
        uint32_t payload_size = s->frag.total_len;

        netpkt_t* reassembled = net_fragbuf_take_packet(&s->frag);
        if (!reassembled) {
            reass_free(s);
            return;
        }

        if (!ipv6_skip_ext_headers(reassembled, &inner_nh, &payload_off, &payload_size)) {
            netpkt_unref(reassembled);
            reass_free(s);
            return;
        }

        if (inner_nh == PROTO_ICMPV6) {
            netpkt_t* l4pkt = netpkt_view(reassembled, payload_off, payload_size);
            if (l4pkt) icmpv6_input(ifindex, ip6->src, ip6->dst, ip6->hop_limit, src_mac, l4pkt);
            netpkt_unref(reassembled);
            reass_free(s);
            return;
        }

        l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
        if (!l2) {
            netpkt_unref(reassembled);
            reass_free(s);
            return;
        }

        l3_ipv6_interface_t* cand[MAX_IPV6_PER_INTERFACE];
        int ccount = 0;
        for (int x = 0; x < MAX_IPV6_PER_INTERFACE; x++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[x];
            if (!ipv6_l3_is_active(v6)) continue;
            cand[ccount++] = v6;
        }
        if (ccount == 0) {
            netpkt_unref(reassembled);
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
                netpkt_unref(reassembled);
                reass_free(s);
                return;
            }

            for (int i = 0; i < ccount; i++) {
                l3_ipv6_interface_t* v6 = cand[i];
                if (!ipv6_is_linklocal(v6->ip) && ipv6_is_linkscope_mcast(ip6->dst)) continue;
                netpkt_t* l4pkt = netpkt_view(reassembled, payload_off, payload_size);
                if (!l4pkt) continue;
                if (inner_nh == PROTO_UDP) udp_input(IP_VER6, ip6->src, ip6->dst, v6->l3_id, l4pkt);
                else if (inner_nh == PROTO_TCP) tcp_input(IP_VER6, ip6->src, ip6->dst, v6->l3_id, l4pkt);
                else netpkt_unref(l4pkt);
            }

            netpkt_unref(reassembled);
            reass_free(s);
            return;
        }

        int match_count = 0;
        l3_id_t match_l3id = 0;
        for (int i = 0; i < ccount; i++) {
            if (ipv6_cmp(cand[i]->ip, ip6->dst) == 0){
                match_count++;
                if (match_count == 1) match_l3id = cand[i]->l3_id;
            }
        }

        if (match_count >= 1) {
            netpkt_t* l4pkt = netpkt_view(reassembled, payload_off, payload_size);
            if (l4pkt) {
                if (inner_nh == PROTO_TCP) tcp_input(IP_VER6, ip6->src, ip6->dst, match_l3id, l4pkt);
                else if (inner_nh == PROTO_UDP) udp_input(IP_VER6, ip6->src, ip6->dst, match_l3id, l4pkt);
                else netpkt_unref(l4pkt);
            }
        }
        netpkt_unref(reassembled);
        reass_free(s);
        return;
    }

    if (nh == PROTO_ICMPV6) {
        netpkt_t* l4pkt = netpkt_view(pkt, l4_off, l4_len);
        if (l4pkt) icmpv6_input(ifindex, ip6->src, ip6->dst, ip6->hop_limit, src_mac, l4pkt);
        return;
    }

    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return;

    l3_ipv6_interface_t* cand[MAX_IPV6_PER_INTERFACE];
    int ccount = 0;
    for (int s = 0; s < MAX_IPV6_PER_INTERFACE; ++s) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[s];
        if (!ipv6_l3_is_active(v6)) continue;
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
            if (!ipv6_is_linklocal(v6->ip) && ipv6_is_linkscope_mcast(ip6->dst))
                continue;

            switch (nh) {
            netpkt_t* l4pkt;
            case PROTO_UDP:
                l4pkt = netpkt_view(pkt, l4_off, l4_len);
                if (l4pkt) udp_input(IP_VER6, ip6->src, ip6->dst, v6->l3_id, l4pkt);
                break;
            case PROTO_TCP:
                l4pkt = netpkt_view(pkt, l4_off, l4_len); 
                if (l4pkt) tcp_input(IP_VER6, ip6->src, ip6->dst, v6->l3_id, l4pkt);
                break;
            default:
                break;
            }
        }
        return;
    }

    int match_count = 0;
    l3_id_t match_l3id = 0;
    for (int i = 0; i < ccount; ++i) {
        if (ipv6_cmp(cand[i]->ip, ip6->dst) == 0) {
            match_count++;
            if (match_count == 1) match_l3id = cand[i]->l3_id;
        }
    }

    if (match_count >= 1) {
        switch (nh) {
        netpkt_t* l4pkt;
        case PROTO_TCP:
            l4pkt = netpkt_view(pkt, l4_off, l4_len);
            if (l4pkt) tcp_input(IP_VER6, ip6->src, ip6->dst, match_l3id, l4pkt);
            break;
        case PROTO_UDP:
            l4pkt = netpkt_view(pkt, l4_off, l4_len);
            if (l4pkt) udp_input(IP_VER6, ip6->src, ip6->dst, match_l3id, l4pkt);
            break;
        default:
            break;
        }
        return;
    }
}