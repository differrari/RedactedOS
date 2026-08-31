#include "ndp.h"
#include "eth.h"
#include "link_utils.h"
#include "networking/internet_layer/icmpv6.h"
#include "std/memory.h"
#include "std/string.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/internet_layer/ipv6_route.h"
#include "networking/internet_layer/mld.h"
#include "net/checksums.h"
#include "syscalls/syscalls.h"
#include "networking/network.h"
#include "process/scheduler.h"
#include "math/rng.h"
#include "random/random.h"

typedef struct {
    uint8_t ip[16];
    uint32_t lifetime_ms;
    int8_t preference;
    uint8_t failed;
    uint8_t used;
} ndp_default_router_t;

#define NDP_DEFAULT_ROUTER_MAX 8

typedef struct {
    ndp_entry_t entries[NDP_TABLE_MAX];
    ndp_default_router_t routers[NDP_DEFAULT_ROUTER_MAX];
    uint32_t base_reachable_time_ms;
    uint32_t reachable_time_ms;
    uint32_t retrans_timer_ms;
    uint32_t reachable_recalc_ms;
    uint8_t router_rr;
} ndp_table_impl_t;

#define NDP_DEFAULT_REACHABLE_TIME_MS 30000u
#define NDP_DEFAULT_RETRANS_TIMER_MS 1000u
#define NDP_REACHABLE_RECALC_MS 7200000u
#define NDP_DELAY_FIRST_PROBE_TIME_MS 5000u
#define NDP_MAX_PROBES 3u
#define NDP_MAX_RA_REACHABLE_TIME_MS 3600000u

enum {
    NDP_OPT_SOURCE_LLADDR = 1,
    NDP_OPT_TARGET_LLADDR = 2,
    NDP_OPT_PREFIX_INFO = 3,
    NDP_OPT_MTU = 5,
    NDP_OPT_RDNSS = 25
};

static rng_t g_rng;

typedef struct __attribute__((packed)) {
    icmpv6_hdr_t hdr;
    uint32_t rsv;
    uint8_t target[16];
} icmpv6_ns_t;

typedef struct __attribute__((packed)) {
    icmpv6_hdr_t hdr;
    uint32_t flags;
    uint8_t target[16];
} icmpv6_na_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
    uint8_t mac[6];
} icmpv6_opt_lladdr_t;

typedef struct __attribute__((packed)) {
    icmpv6_hdr_t hdr;
    uint8_t cur_hop_limit;
    uint8_t flags;
    uint16_t router_lifetime;
    uint32_t reachable_time;
    uint32_t retrans_timer;
} icmpv6_ra_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
    uint8_t prefix_length;
    uint8_t flags;
    uint32_t valid_lifetime;
    uint32_t preferred_lifetime;
    uint32_t reserved2;
    uint8_t prefix[16];
} ndp_opt_prefix_info_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
    uint16_t reserved;
    uint32_t mtu;
} ndp_opt_mtu_t;

static uint8_t g_rs_tries[MAX_L2_INTERFACES];
static uint32_t g_rs_timer_ms[MAX_L2_INTERFACES];

static bool ndp_read_lladdr_option(netpkt_t * pkt, uint32_t off, uint32_t len, uint8_t type, uint8_t mac[6], bool* found) {
    if (!pkt || !found) return false;
    *found = false;

    while (len) {
        if (len < 2) return false;

        uint8_t head[2];
        if (!netpkt_copyout(pkt, off, head, sizeof(head))) return false;
        if (!head[1]) return false;

        uint32_t size = (uint32_t)head[1]*8;
        if (size > len) return false;

        if (head[0] == type) {
            if (size != sizeof(icmpv6_opt_lladdr_t)) return false;
            if (!*found) {
                icmpv6_opt_lladdr_t opt;
                if (!netpkt_copyout(pkt, off, &opt, sizeof(opt))) return false;
                if (mac) mac_copy(mac, opt.mac);
                *found = true;
            }
        }

        off += size;
        len -= size;
    }

    return true;
}

static void ndp_flush_pending(uint8_t ifindex, ndp_entry_t* e) {
    if (!e || !e->pending) return;

    for (int i = 0; i < e->pending_len; i++) {
        netpkt_t* pkt = e->pending[i];
        e->pending[i] = 0;
        if (pkt) eth_send_frame_on(ifindex, ETHERTYPE_IPV6, e->mac, pkt);
    }

    release(e->pending);
    e->pending = 0;
    e->pending_len = 0;
    e->pending_bytes = 0;
}

static void ndp_mark_neighbor_observed(ndp_table_impl_t* t, ndp_entry_t *e, bool solicited) {
    if (!e) return;
    if (solicited) {
        e->state = NDP_STATE_REACHABLE;
        e->timer_ms = t && t->reachable_time_ms ? t->reachable_time_ms : NDP_DEFAULT_REACHABLE_TIME_MS;
    } else {
        e->state = NDP_STATE_STALE;
        e->timer_ms = 0;
    }
}

static void make_random_iid(uint8_t out_iid[8]) {
    uint64_t x = 0;

    do x = rng_next64(&g_rng);
    while (x == 0);

    out_iid[0] = (uint8_t)((x >> 56) & 0xFF);
    out_iid[1] = (uint8_t)((x >> 48) & 0xFF);
    out_iid[2] = (uint8_t)((x >> 40) & 0xFF);
    out_iid[3] = (uint8_t)((x >> 32) & 0xFF);
    out_iid[4] = (uint8_t)((x >> 24) & 0xFF);
    out_iid[5] = (uint8_t)((x >> 16) & 0xFF);
    out_iid[6] = (uint8_t)((x >> 8) & 0xFF);
    out_iid[7] = (uint8_t)(x & 0xFF);
}

static void handle_dad_failed(l3_ipv6_interface_t* v6) {
    if (!v6) return;

    uint8_t iid[8];
    uint8_t new_ip[16];

    make_random_iid(iid);

    if (ipv6_is_linklocal(v6->ip)) {
        new_ip[0] = 0xFE;
        new_ip[1] = 0x80;
        memset(new_ip + 2, 0, 6);
        memcpy(new_ip + 8, iid, 8);

        (void)l3_ipv6_update(v6->l3_id, new_ip, 64, (const uint8_t[16]){0}, v6->cfg, v6->kind);
        (void)ndp_request_dad_on(v6->l2 ? v6->l2->ifindex : 0, new_ip);
        return;
    }

    if (v6->prefix_len != 64) {
        ipv6_cpy(new_ip, v6->ip);
        memcpy(new_ip + 8, iid, 8);

        (void)l3_ipv6_update(v6->l3_id, new_ip, v6->prefix_len, v6->gateway, v6->cfg, v6->kind);
        (void)ndp_request_dad_on(v6->l2 ? v6->l2->ifindex : 0, new_ip);
        return;
    }

    if (!ipv6_is_unspecified(v6->prefix)) ipv6_cpy(new_ip, v6->prefix);
    else {
        ipv6_cpy(new_ip, v6->ip);
        memset(new_ip + 8, 0, 8);
    }

    memcpy(new_ip + 8, iid, 8);

    (void)l3_ipv6_update(v6->l3_id, new_ip, 64, v6->gateway, v6->cfg, v6->kind);
    (void)ndp_request_dad_on(v6->l2 ? v6->l2->ifindex : 0, new_ip);
}

static int ndp_find_slot(ndp_table_impl_t* t, const uint8_t ip[16]) {
    if (!t) return -1;

    for (int i = 0; i < NDP_TABLE_MAX; i++) {
        if (!t->entries[i].ttl_ms) continue;
        if (ipv6_cmp(t->entries[i].ip, ip) == 0) return i;
    }

    return -1;
}

static int ndp_find_free(ndp_table_impl_t* t) {
    if (!t) return -1;
    for (int i = 0; i < NDP_TABLE_MAX; i++) if (t->entries[i].ttl_ms == 0 && t->entries[i].state == NDP_STATE_UNUSED) return i;
    return -1;
}

static bool ndp_select_default_router(ndp_table_impl_t* t, const uint8_t current[16], uint8_t out[16]) {
    if (!out) return false;
    memset(out, 0, 16);
    if (!t) return false;

    int best = -1;
    int8_t best_pref = -2;
    for (int i = 0; i < NDP_DEFAULT_ROUTER_MAX; i++) {
        ndp_default_router_t* r = &t->routers[i];
        if (!r->used || !r->lifetime_ms || r->failed) continue;

        int neighbor = ndp_find_slot(t, r->ip);
        if (neighbor >= 0 && t->entries[neighbor].state == NDP_STATE_INCOMPLETE) continue;

        bool is_current = current && !ipv6_is_unspecified(current) && ipv6_cmp(r->ip, current) == 0;
        if (r->preference > best_pref || (r->preference == best_pref && is_current)) {
            best = i;
            best_pref = r->preference;
        }
    }

    if (best < 0) {
        int8_t fallback_pref = -2;
        for (int i = 0; i < NDP_DEFAULT_ROUTER_MAX; i++) {
            ndp_default_router_t* r = &t->routers[i];
            if (!r->used || !r->lifetime_ms) continue;
            if (r->preference > fallback_pref) fallback_pref = r->preference;
        }

        if (fallback_pref > -2) {
            for (int n = 0; n < NDP_DEFAULT_ROUTER_MAX; n++) {
                int i = (t->router_rr + n) % NDP_DEFAULT_ROUTER_MAX;
                ndp_default_router_t* r = &t->routers[i];
                if (!r->used || !r->lifetime_ms || r->preference != fallback_pref) continue;
                best = i;
                t->router_rr = (uint8_t)(((i)+ 1) % NDP_DEFAULT_ROUTER_MAX);
                break;
            }
        }
    }

    if (best < 0) return false;
    ipv6_cpy(out, t->routers[best].ip);
    return true;
}

static void handle_lifetimes(uint32_t now_ms, l3_ipv6_interface_t* v6) {
    if (!v6) return;
    if (ipv6_is_placeholder_gua(v6->ip)) return;
    if (ipv6_is_unspecified(v6->ip)) return;
    if (ipv6_is_linklocal(v6->ip)) return;
    if (!v6->ra_last_update_ms) return;

    uint32_t elapsed_ms = now_ms - v6->ra_last_update_ms;

    if (v6->preferred_lifetime && v6->preferred_lifetime != 0xFFFFFFFFu) {
        uint64_t pref_ms = (uint64_t)v6->preferred_lifetime * 1000ull;
        if ((uint64_t)elapsed_ms >= pref_ms) v6->preferred_lifetime = 0;
    }

    if (v6->valid_lifetime == 0xFFFFFFFFu) return;

    uint64_t valid_ms = (uint64_t)v6->valid_lifetime * 1000ull;
    if ((uint64_t)elapsed_ms >= valid_ms) {
        if (!l3_ipv6_remove_from_interface(v6->l3_id)) (void)l3_ipv6_update(v6->l3_id, v6->ip, v6->prefix_len, v6->gateway, IPV6_CFG_DISABLE, v6->kind);
    }
}

static void apply_ra_policy(uint32_t now_ms, l2_interface_t* l2) {
    if (!l2) return;

    uint8_t ifx = l2->ifindex;
    if (!ifx || ifx > MAX_L2_INTERFACES) return;
    ndp_table_impl_t* t = (ndp_table_impl_t*)l2->nd_table;

    int has_lla_ok = 0;
    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (!ipv6_l3_is_ready(v6)) continue;
        if (!ipv6_is_linklocal(v6->ip)) continue;
        has_lla_ok = 1;
        break;
    }

    if (!has_lla_ok) return;

    uint8_t default_router[16] = {0};
    (void)ndp_select_default_router(t, NULL, default_router);

    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (!ipv6_l3_is_active(v6)) continue;
        if (!(v6->kind & IPV6_ADDRK_GLOBAL)) continue;
        if (!(v6->cfg & (IPV6_CFG_SLAAC | IPV6_CFG_DHCPV6))) continue;
        if (!v6->ra_has) continue;
        if (ipv6_is_unspecified(v6->prefix)) continue;
        uint8_t m = (v6->ra_flags & RA_FLAG_M) ? 1u : 0u;
        uint8_t o = (v6->ra_flags & RA_FLAG_O) ? 1u : 0u;
        if (!v6->ra_autonomous) {
            if (m) {
                v6->dhcpv6_stateless = 0;
                v6->dhcpv6_stateless_done = 0;

                if (v6->cfg != IPV6_CFG_DHCPV6 || ipv6_is_placeholder_gua(v6->ip)) {
                    (void)l3_ipv6_update(v6->l3_id, (const uint8_t[16]){0}, 0, default_router, IPV6_CFG_DHCPV6, v6->kind);
                } else {
                    (void)l3_ipv6_update(v6->l3_id, v6->ip, v6->prefix_len, default_router, IPV6_CFG_DHCPV6, v6->kind);
                }
            } else {
                v6->dhcpv6_stateless = o ? 1 : 0;
                v6->dhcpv6_stateless_done = 0;
            }

            continue;
        }
        v6->dhcpv6_stateless = o ? 1 : 0;
        v6->dhcpv6_stateless_done = 0;

        ipv6_cfg_t ra_cfg = o ? IPV6_CFG_STATELESS : IPV6_CFG_SLAAC;
        if (v6->cfg != ra_cfg) {
            uint8_t ph[16];

            ipv6_make_placeholder_gua(ph);
            (void)l3_ipv6_update(v6->l3_id, ph, 64, default_router, ra_cfg, v6->kind);
        }

        if (ipv6_is_placeholder_gua(v6->ip)) {
            uint8_t iid[8];
            uint8_t ip[16];

            make_random_iid(iid);
            ipv6_cpy(ip, v6->prefix);
            memcpy(ip + 8, iid, 8);

            (void)l3_ipv6_update(v6->l3_id, ip, 64, v6->gateway, ra_cfg, v6->kind);

            v6->timestamp_created = now_ms;
            memcpy(v6->interface_id, ip + 8, 8);

            if (v6->dad_state == IPV6_DAD_NONE && !v6->dad_requested) (void)ndp_request_dad_on(ifx, ip);
            continue;
        }

        (void)l3_ipv6_update(v6->l3_id, v6->ip, v6->prefix_len, default_router, ra_cfg, v6->kind);

        v6->timestamp_created = now_ms;
        memcpy(v6->interface_id, v6->ip + 8, 8);
    }
}

static void ndp_on_ra(uint8_t ifindex, const uint8_t prefix[16], uint8_t prefix_len, uint32_t valid_lft, uint32_t preferred_lft, bool autonomous, uint8_t ra_flags) {
    if (!ifindex) return;
    if (prefix_len != 64) return;
    if (ipv6_is_unspecified(prefix) || ipv6_is_multicast(prefix) || ipv6_is_linklocal(prefix)) return;

    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return;
    ndp_table_impl_t* t = (ndp_table_impl_t*)l2->nd_table;

    uint32_t now_ms = get_time();
    l3_ipv6_interface_t* slot = NULL;

    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (!v6) continue;
        if (v6->cfg == IPV6_CFG_DISABLE) continue;
        if (v6->kind != IPV6_ADDRK_GLOBAL) continue;
        if (!(v6->cfg & (IPV6_CFG_SLAAC | IPV6_CFG_DHCPV6))) continue;

        if (!ipv6_is_unspecified(v6->prefix)) {
            if (ipv6_common_prefix_len(v6->prefix, prefix) >= 64) {
                slot = v6;
                break;
            }
        } else {
            if (ipv6_is_placeholder_gua(v6->ip)) {
                slot = v6;
                break;
            }

            if (!ipv6_is_unspecified(v6->ip) && !ipv6_is_multicast(v6->ip) && !ipv6_is_linklocal(v6->ip)) {
                if (ipv6_common_prefix_len(v6->ip, prefix) >= 64) {
                    slot = v6;
                    break;
                }
            }
        }
    }

    if (!slot) {
        uint8_t ph[16];
        ipv6_make_placeholder_gua(ph);

        l3_id_t id = l3_ipv6_add_to_interface(ifindex, ph, 64, (const uint8_t[16]){0}, (ra_flags & RA_FLAG_O) ? IPV6_CFG_STATELESS : IPV6_CFG_SLAAC, IPV6_ADDRK_GLOBAL);
        if (!id) return;

        slot = l3_ipv6_find_by_id(id);
        if (!slot) return;
    }

    slot->ra_has = 1;
    slot->ra_autonomous = autonomous ? 1 : 0;
    slot->ra_last_update_ms = now_ms;
    slot->ra_flags = ra_flags;

    ipv6_cpy(slot->prefix, prefix);

    slot->valid_lifetime = valid_lft;
    slot->preferred_lifetime = preferred_lft;

    uint8_t gw[16] = {0};
    (void)ndp_select_default_router(t, slot->gateway, gw);
    l3_ipv6_update(slot->l3_id, slot->ip, slot->prefix_len, gw, slot->cfg, slot->kind);
    apply_ra_policy(now_ms, l2);

    if (ipv6_is_unspecified(slot->ip)) slot->timestamp_created = now_ms;

    if (!ipv6_is_placeholder_gua(slot->ip) && !ipv6_is_unspecified(slot->ip)) memcpy(slot->interface_id, slot->ip + 8, 8);
}

static void ndp_entry_clear(ndp_entry_t* e) {
    if (!e) return;
    if (e->pending) {
        for (int i = 0; i < e->pending_len; i++) if (e->pending[i]) netpkt_unref(e->pending[i]);
        release(e->pending);
    }
    memset(e, 0, sizeof(*e));
    e->state = NDP_STATE_UNUSED;
}

ndp_table_t* ndp_table_create(void) {
    ndp_table_impl_t* t = (ndp_table_impl_t*)zalloc(sizeof(ndp_table_impl_t));
    if (!t) return 0;

    t->base_reachable_time_ms = NDP_DEFAULT_REACHABLE_TIME_MS;
    t->reachable_time_ms = NDP_DEFAULT_REACHABLE_TIME_MS;
    t->retrans_timer_ms = NDP_DEFAULT_RETRANS_TIMER_MS;
    t->reachable_recalc_ms = NDP_REACHABLE_RECALC_MS;

    return (ndp_table_t*)t;
}

void ndp_table_destroy(ndp_table_t* t) {
    if (!t) return;
    ndp_table_impl_t* impl = (ndp_table_impl_t*)t;
    for (int i = 0; i < NDP_TABLE_MAX; i++) ndp_entry_clear(&impl->entries[i]);
    release(t);
}

static int ndp_find_replacement(ndp_table_impl_t* t) {
    uint32_t best_ttl = 0xFFFFFFFFu;
    int best = -1;
    if (!t) return -1;

    for (int i = 0; i < NDP_TABLE_MAX; i++) {
        ndp_entry_t* e = &t->entries[i];
        if (e->pending_len) continue;
        if (e->state == NDP_STATE_UNUSED || e->ttl_ms == 0) return i;
        if (e->static_entry) continue;
        if (e->ttl_ms < best_ttl) {
            best_ttl = e->ttl_ms;
            best = i;
        }
    }

    return best;
}

static void ndp_sync_default_routes(uint8_t ifindex, ndp_table_impl_t* t) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2 || !t) return;

    const uint8_t* current = NULL;
    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (!v6 || v6->is_localhost || !(v6->kind & IPV6_ADDRK_GLOBAL)) continue;
        if (!(v6->cfg & (IPV6_CFG_SLAAC | IPV6_CFG_DHCPV6))) continue;
        current = v6->gateway;
        break;
    }

    uint8_t selected[16] = {0};
    ndp_select_default_router(t, current, selected);

    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (!v6 || v6->is_localhost || !(v6->kind & IPV6_ADDRK_GLOBAL)) continue;
        if (!(v6->cfg & (IPV6_CFG_SLAAC | IPV6_CFG_DHCPV6))) continue;
        if (ipv6_cmp(v6->gateway, selected) == 0) continue;
        (void)l3_ipv6_update(v6->l3_id, v6->ip, v6->prefix_len, selected, v6->cfg, v6->kind);
    }
}

void ndp_table_put_for_l2(uint8_t ifindex, const uint8_t ip[16], const uint8_t mac[6], uint32_t ttl_ms, bool router, bool is_static) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    ndp_table_impl_t* t = l2 ? (ndp_table_impl_t*)l2->nd_table : NULL;
    if (!t) return;

    int idx = ndp_find_slot(t, ip);
    if (idx < 0) idx = ndp_find_free(t);

    if (idx < 0) idx = ndp_find_replacement(t);
    if (idx < 0) return;

    ndp_entry_t* e = &t->entries[idx];
    if (e->state != NDP_STATE_UNUSED && ipv6_cmp(e->ip, ip) == 0 && e->static_entry && !is_static) return;
    if (e->state != NDP_STATE_UNUSED && ipv6_cmp(e->ip, ip) != 0) ndp_entry_clear(e);
    ipv6_cpy(e->ip, ip);

    if (mac) {
        mac_copy(e->mac, mac);
        e->state = NDP_STATE_REACHABLE;
        e->timer_ms = t->reachable_time_ms;
    }

    if (is_static) ttl_ms = UINT32_MAX;
    else if (ttl_ms == 0) {
        ttl_ms = t->reachable_time_ms * 4;
    }

    e->ttl_ms = ttl_ms;
    e->is_router = router ? 1 : 0;
    e->static_entry = is_static ? 1 : 0;
    e->probes_sent = 0;

    ndp_flush_pending(ifindex, e);
}

bool ndp_table_get_for_l2(uint8_t ifindex, const uint8_t ip[16], uint8_t mac_out[6]) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    ndp_table_impl_t* t = l2 ? (ndp_table_impl_t*)l2->nd_table : NULL;
    if (!t) return false;

    for (int i = 0; i < NDP_TABLE_MAX; i++) {
        ndp_entry_t* e = &t->entries[i];
        if (!e->ttl_ms) continue;
        if (e->state == NDP_STATE_UNUSED) continue;
        if (e->state == NDP_STATE_INCOMPLETE) continue;
        if (ipv6_cmp(e->ip, ip) != 0) continue;

        mac_copy(mac_out, e->mac);
        if (e->state == NDP_STATE_STALE) {
            e->state = NDP_STATE_DELAY;
            e->timer_ms = NDP_DELAY_FIRST_PROBE_TIME_MS;
        }
        return true;
    }

    return false;
}

bool ndp_table_delete_for_l2(uint8_t ifindex, const uint8_t ip[16]) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    ndp_table_impl_t* t = l2 ? (ndp_table_impl_t*)l2->nd_table : NULL;
    if (!t || !ip) return false;
    int idx = ndp_find_slot(t, ip);
    if (idx < 0) return false;
    ndp_entry_clear(&t->entries[idx]);
    return true;
}

uint32_t ndp_table_dump_for_l2(uint8_t ifindex, ndp_entry_t* out, uint32_t out_cap) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    ndp_table_impl_t* t = l2 ? (ndp_table_impl_t*)l2->nd_table : NULL;
    if (!t || !out || !out_cap) return 0;
    uint32_t n = 0;
    for (int i = 0; i < NDP_TABLE_MAX && n < out_cap; i++) {
        ndp_entry_t* e = &t->entries[i];
        if (e->state == NDP_STATE_UNUSED) continue;
        out[n++] = *e;
    }
    return n;
}

uint32_t ndp_default_router_lifetime_for_l2(uint8_t ifindex, const uint8_t ip[16]) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    ndp_table_impl_t* t = l2 ? (ndp_table_impl_t*)l2->nd_table : NULL;
    if (!t || !ip) return 0;
    for (int i = 0; i < NDP_DEFAULT_ROUTER_MAX; i++) if (t->routers[i].used && ipv6_cmp(t->routers[i].ip, ip) == 0) return t->routers[i].lifetime_ms;
    return 0;
}

static bool ndp_send_na_on(uint8_t ifindex, const uint8_t dst_ip[16], const uint8_t src_ip[16], const uint8_t target_ip[16], const uint8_t dst_mac_in[6], const uint8_t my_mac[6], uint8_t solicited) {
    if (!my_mac) return false;
    if (!ipv6_is_multicast(dst_ip) && !dst_mac_in) return false;

    uint32_t plen = (uint32_t)(sizeof(icmpv6_na_t) + sizeof(icmpv6_opt_lladdr_t));
    uintptr_t buf = (uintptr_t)zalloc(plen ? plen : 1u);
    if (!buf) return false;

    icmpv6_na_t* na = (icmpv6_na_t*)buf;
    na->hdr.type = 136;
    na->hdr.code = 0;
    na->hdr.checksum = 0;

    uint32_t flags = 0;
    if (solicited) flags |= (1u << 30);
    flags |= (1u << 29);
    na->flags = bswap32(flags);

    ipv6_cpy(na->target, target_ip);

    icmpv6_opt_lladdr_t* opt = (icmpv6_opt_lladdr_t*)(buf + sizeof(icmpv6_na_t));
    opt->type = 2;
    opt->length = 1;
    mac_copy(opt->mac, my_mac);

    na->hdr.checksum = bswap16(checksum16_pipv6(src_ip, dst_ip, PROTO_ICMPV6, (const uint8_t*)buf, plen));

    uint8_t dst_mac[6];
    if (ipv6_is_multicast(dst_ip)) ipv6_multicast_mac(dst_ip, dst_mac);
    else mac_copy(dst_mac, dst_mac_in);

    bool ok = icmpv6_send_on_l2(ifindex, dst_ip, src_ip, dst_mac, (const void*)buf, plen, 255);

    release((void*)buf);
    return ok;
}

static void ndp_send_ns_on(uint8_t ifindex, const uint8_t target_ip[16], const uint8_t src_ip[16], const uint8_t unicast_mac[6]) {
    bool dad = ipv6_is_unspecified(src_ip);
    const uint8_t* mac = dad ? NULL : network_get_mac(ifindex);
    if (!dad && !mac) return;

    uint32_t plen = (uint32_t)sizeof(icmpv6_ns_t) + (mac ? (uint32_t)sizeof(icmpv6_opt_lladdr_t) : 0u);
    uintptr_t buf = (uintptr_t)zalloc(plen ? plen : 1u);
    if (!buf) return;

    icmpv6_ns_t* ns = (icmpv6_ns_t*)buf;
    ns->hdr.type = 135;
    ns->hdr.code = 0;
    ns->hdr.checksum = 0;
    ns->rsv = 0;

    ipv6_cpy(ns->target, target_ip);

    if (mac) {
        icmpv6_opt_lladdr_t* opt = (icmpv6_opt_lladdr_t*)(buf + sizeof(icmpv6_ns_t));
        opt->type = 1;
        opt->length = 1;
        mac_copy(opt->mac, mac);
    }

    uint8_t dst_ip[16];
    if (unicast_mac) ipv6_cpy(dst_ip, target_ip);
    else ipv6_make_multicast(2, IPV6_MCAST_SOLICITED_NODE, target_ip, dst_ip);

    ns->hdr.checksum = bswap16(checksum16_pipv6(src_ip, dst_ip, PROTO_ICMPV6, (const uint8_t*)buf, plen));

    uint8_t dst_mac[6];
    if (unicast_mac) mac_copy(dst_mac, unicast_mac);
    else ipv6_multicast_mac(dst_ip, dst_mac);

    icmpv6_send_on_l2(ifindex, dst_ip, src_ip, dst_mac, (const void*)buf, plen, 255);
    release((void*)buf);
}

static void ndp_send_rs_on(uint8_t ifindex) {
    uint8_t src_ip[16] = {0};
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);

    if (l2) {
        for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[i];
            if (!v6) continue;
            if (v6->cfg == IPV6_CFG_DISABLE) continue;
            if (v6->dad_state == IPV6_DAD_FAILED) continue;

            if (ipv6_is_linklocal(v6->ip)) {
                ipv6_cpy(src_ip, v6->ip);
                break;
            }
        }
    }

    uint8_t dst_ip[16];
    ipv6_make_multicast(2, IPV6_MCAST_ALL_ROUTERS, 0, dst_ip);

    typedef struct __attribute__((packed)) {
        icmpv6_hdr_t hdr;
        uint32_t reserved;
    } icmpv6_rs_t;

    const uint8_t* mac = ipv6_is_unspecified(src_ip) ? NULL : network_get_mac(ifindex);
    uint32_t plen = (uint32_t)sizeof(icmpv6_rs_t) + (mac ? (uint32_t)sizeof(icmpv6_opt_lladdr_t) : 0u);
    uintptr_t buf = (uintptr_t)zalloc(plen ? plen : 1u);
    if (!buf) return;

    icmpv6_rs_t* rs = (icmpv6_rs_t*)buf;
    rs->hdr.type = 133;
    rs->hdr.code = 0;
    rs->hdr.checksum = 0;
    rs->reserved = 0;

    if (mac) {
        icmpv6_opt_lladdr_t* opt = (icmpv6_opt_lladdr_t*)(buf + sizeof(icmpv6_rs_t));
        opt->type = 1;
        opt->length = 1;
        mac_copy(opt->mac, mac);
    }

    rs->hdr.checksum = bswap16(checksum16_pipv6(src_ip, dst_ip, PROTO_ICMPV6, (const uint8_t*)buf, plen));

    uint8_t dst_mac[6];
    ipv6_multicast_mac(dst_ip, dst_mac);

    icmpv6_send_on_l2(ifindex, dst_ip, src_ip, dst_mac, (const void*)buf, plen, 255);
    release((void*)buf);
}

static void ndp_send_probe(uint8_t ifindex, ndp_entry_t* e) {
    uint8_t src_ip[16] = {0};
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);

    if (l2) {
        for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[i];
            if (!ipv6_l3_is_ready(v6)) continue;

            if (ipv6_is_linklocal(v6->ip)) {
                ipv6_cpy(src_ip, v6->ip);
                break;
            }

            if (ipv6_is_unspecified(src_ip) && !ipv6_is_unspecified(v6->ip))
                ipv6_cpy(src_ip, v6->ip);
        }
    }

    ndp_send_ns_on(ifindex, e->ip, src_ip, e->state == NDP_STATE_PROBE ? e->mac : NULL);
}

static void ndp_table_tick_for_l2(uint8_t ifindex, uint32_t ms) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    ndp_table_impl_t* t = l2 ? (ndp_table_impl_t*)l2->nd_table : NULL;
    if (!t) return;

    if (t->reachable_recalc_ms <= ms) {
        uint32_t low = t->base_reachable_time_ms / 2;
        uint32_t high = t->base_reachable_time_ms + t->base_reachable_time_ms / 2;
        t->reachable_time_ms = rng_between32(&g_rng, low, high+1);
        if (!t->reachable_time_ms) t->reachable_time_ms = 1;
        t->reachable_recalc_ms = NDP_REACHABLE_RECALC_MS;
    } else t->reachable_recalc_ms -= ms;

    bool routers_changed = false;
    for (int i = 0; i < NDP_DEFAULT_ROUTER_MAX; i++) {
        ndp_default_router_t* r = &t->routers[i];
        if (!r->used) continue;
        if (r->lifetime_ms <= ms) {
            memset(r, 0, sizeof(*r));
            routers_changed = true;
        } else r->lifetime_ms -= ms;
    }
    if (routers_changed) ndp_sync_default_routes(ifindex, t);

    for (int i = 0; i < NDP_TABLE_MAX; i++) {
        ndp_entry_t* e =&t->entries[i];

        if (!e->ttl_ms) {
            if (e->state != NDP_STATE_UNUSED) ndp_entry_clear(e);
            continue;
        }

        if (e->static_entry) continue;
        if (e->ttl_ms <= ms) {
            ndp_entry_clear(e);
            continue;
        }

        e->ttl_ms -= ms;

        if (e->timer_ms) {
            if (e->timer_ms <= ms)e->timer_ms = 0;
            else e->timer_ms -= ms;
        }

        switch (e->state) {
        case NDP_STATE_INCOMPLETE:
            if (e->timer_ms == 0) {
                if (e->probes_sent < NDP_MAX_PROBES) {
                    e->probes_sent++;
                    e->timer_ms = t->retrans_timer_ms;
                    ndp_send_probe(ifindex, e);
                } else {
                    if (e->is_router) {
                        bool changed = false;
                        for (int r = 0; r < NDP_DEFAULT_ROUTER_MAX; r++) {
                            if (!t->routers[r].used || ipv6_cmp(t->routers[r].ip, e->ip) != 0) continue;
                            if (!t->routers[r].failed) {
                                t->routers[r].failed = 1;
                                changed = true;
                            }
                            break;
                        }
                        if (changed) ndp_sync_default_routes(ifindex, t);
                    }
                    ndp_entry_clear(e);
                }
            }
            break;

        case NDP_STATE_REACHABLE:
            if (e->timer_ms == 0) e->state = NDP_STATE_STALE;
            break;

        case NDP_STATE_DELAY:
            if (e->timer_ms == 0) {
                e->state = NDP_STATE_PROBE;
                e->probes_sent = 0;
                e->timer_ms = t->retrans_timer_ms;
                ndp_send_probe(ifindex, e);
            }
            break;

        case NDP_STATE_PROBE:
            if (e->timer_ms == 0) {
                if (e->probes_sent < NDP_MAX_PROBES) {
                    e->probes_sent++;
                    e->timer_ms = t->retrans_timer_ms;
                    ndp_send_probe(ifindex, e);
                } else {
                    if (e->is_router) {
                        bool changed = false;
                        for (int r = 0; r < NDP_DEFAULT_ROUTER_MAX; r++) {
                            if (!t->routers[r].used || ipv6_cmp(t->routers[r].ip, e->ip) != 0) continue;
                            if (!t->routers[r].failed) {
                                t->routers[r].failed = 1;
                                changed = true;
                            }
                            break;
                        }
                        if (changed) ndp_sync_default_routes(ifindex, t);
                    }
                    ndp_entry_clear(e);
                }
            }
            break;

        default:
            break;
        }
    }
}

bool ndp_send_or_queue_on(uint8_t ifindex, const uint8_t next_hop[16], netpkt_t* pkt) {
    if (!next_hop || !pkt || !netpkt_len(pkt)) {
        if (pkt) netpkt_unref(pkt);
        return false;
    }

    uint8_t out_mac[6];
    if (ipv6_is_multicast(next_hop)) {
        ipv6_multicast_mac(next_hop, out_mac);
        return eth_send_frame_on(ifindex, ETHERTYPE_IPV6, out_mac, pkt);
    }

    if (ndp_table_get_for_l2(ifindex, next_hop, out_mac)) return eth_send_frame_on(ifindex, ETHERTYPE_IPV6, out_mac, pkt);

    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    ndp_table_impl_t* t = l2 ? (ndp_table_impl_t*)l2->nd_table : NULL;
    if (!t) {
        netpkt_unref(pkt);
        return false;
    }

    int idx = ndp_find_slot(t, next_hop);
    if (idx < 0) idx = ndp_find_free(t);
    if (idx < 0) idx = ndp_find_replacement(t);
    if (idx < 0) {
        netpkt_unref(pkt);
        return false;
    }

    ndp_entry_t* e = &t->entries[idx];
    if (e->state != NDP_STATE_UNUSED && ipv6_cmp(e->ip, next_hop) != 0) ndp_entry_clear(e);

    uint32_t len = netpkt_len(pkt);
    if (e->pending_len >= NDP_PENDING_MAX || e->pending_bytes + len > NDP_PENDING_MAX_BYTES) {
        netpkt_unref(pkt);
        return false;
    }

    if (!e->pending) {
        e->pending = (netpkt_t**)zalloc(sizeof(netpkt_t*) * NDP_PENDING_MAX);
        if (!e->pending) {
            netpkt_unref(pkt);
            return false;
        }
    }

    ipv6_cpy(e->ip, next_hop);
    mac_clear(e->mac);
    e->ttl_ms = t->reachable_time_ms * 4;
    e->is_router = 0;
    for (int r = 0; r < NDP_DEFAULT_ROUTER_MAX; r++) {
        if (!t->routers[r].used || ipv6_cmp(t->routers[r].ip, next_hop) != 0) continue;
        e->is_router = 1;
        break;
    }
    e->state = NDP_STATE_INCOMPLETE;
    e->pending[e->pending_len] = pkt;
    e->pending_len++;
    e->pending_bytes += len;

    if (!e->probes_sent || !e->timer_ms) {
        e->timer_ms = t->retrans_timer_ms;
        e->probes_sent++;
        ndp_send_probe(ifindex, e);
    }

    return true;
}

bool ndp_request_dad_on(uint8_t ifindex, const uint8_t ip[16]) {
    if (!ifindex || !ip) return false;

    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return false;

    if (ipv6_is_unspecified(ip) || ipv6_is_multicast(ip)) return false;

    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (!v6) continue;
        if (v6->cfg == IPV6_CFG_DISABLE) continue;
        if (ipv6_cmp(v6->ip, ip) != 0) continue;

        v6->dad_state = IPV6_DAD_NONE;
        v6->dad_timer_ms = 0;
        v6->dad_probes_sent = 0;
        v6->dad_requested = 1;

        return true;
    }

    return false;
}

void ndp_input(uint8_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], const uint8_t src_mac[6], netpkt_t* pkt) {
    if (!ifindex || ifindex > MAX_L2_INTERFACES || !src_ip || !dst_ip || !pkt || netpkt_len(pkt) < sizeof(icmpv6_hdr_t)) return;

    uint8_t ifx = ifindex;
    uint32_t icmp_len = netpkt_len(pkt);
    icmpv6_hdr_t hdr;
    if (!netpkt_copyout(pkt, 0, &hdr, sizeof(hdr))) return;
    const icmpv6_hdr_t* h = &hdr;
    if (h->code != 0) return;

    if (h->type == ICMPV6_NEIGHBOR_SOLICIT) {
        if (icmp_len < sizeof(icmpv6_ns_t)) return;

        icmpv6_ns_t ns;
        if (!netpkt_copyout(pkt, 0, &ns, sizeof(ns))) return;
        if (ipv6_is_multicast(ns.target)) return;

        uint8_t slla[6];
        bool has_slla = false;
        uint32_t opt_off = (uint32_t)sizeof(icmpv6_ns_t);
        uint32_t opt_len = icmp_len - opt_off;
        if (!ndp_read_lladdr_option(pkt, opt_off, opt_len, NDP_OPT_SOURCE_LLADDR, slla, &has_slla)) return;

        bool dad = ipv6_is_unspecified(src_ip);
        if (dad) {
            uint8_t solicited_node[16];
            ipv6_make_multicast(2, IPV6_MCAST_SOLICITED_NODE, ns.target, solicited_node);
            if (ipv6_cmp(dst_ip, solicited_node) != 0 || has_slla) return;
        }
        if (has_slla && !mac_is_unicast(slla)) has_slla = false;

        l2_interface_t* l2 = l2_interface_find_by_index(ifx);
        if (!l2) return;

        l3_ipv6_interface_t* self = NULL;

        for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[i];
            if (!v6 || v6->cfg == IPV6_CFG_DISABLE) continue;
            if (ipv6_cmp(v6->ip, ns.target) == 0) {
                self = v6;
                break;
            }
        }

        if (!self) return;

        if (self->dad_state != IPV6_DAD_OK) {
            if (dad && (self->dad_state == IPV6_DAD_IN_PROGRESS || self->dad_requested)) {
                self->dad_state = IPV6_DAD_FAILED;
                self->dad_timer_ms = 0;
                self->dad_probes_sent = 0;
                self->dad_requested = 0;
            }
            return;
        }

        const uint8_t* my_mac = network_get_mac(ifx);
        if (!my_mac) return;

        if (dad) {
            uint8_t all_nodes[16];
            ipv6_make_multicast(2, IPV6_MCAST_ALL_NODES, NULL, all_nodes);
            (void)ndp_send_na_on(ifx, all_nodes, self->ip, ns.target, NULL, my_mac, 0);
            return;
        }

        bool src_is_local = false;
        for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[i];
            if (!v6 || v6->cfg == IPV6_CFG_DISABLE) continue;
            if (ipv6_cmp(v6->ip, src_ip) == 0) {
                src_is_local = true;
                break;
            }
        }

        if (!src_is_local && has_slla) {
            ndp_table_impl_t* t = (ndp_table_impl_t*)l2->nd_table;
            if (t) {
                int idx = ndp_find_slot(t, src_ip);
                if (idx < 0) idx = ndp_find_free(t);
                if (idx < 0) idx = ndp_find_replacement(t);

                if (idx >= 0) {
                    ndp_entry_t* e = &t->entries[idx];
                    if (!(e->state != NDP_STATE_UNUSED && ipv6_cmp(e->ip, src_ip) == 0 && e->static_entry)) {
                        if (e->state != NDP_STATE_UNUSED && ipv6_cmp(e->ip, src_ip) != 0) ndp_entry_clear(e);
                        ipv6_cpy(e->ip, src_ip);
                        if (!mac_equal(e->mac, slla)) {
                            mac_copy(e->mac, slla);
                            e->state = NDP_STATE_STALE;
                            e->timer_ms = 0;
                        } else if (e->state == NDP_STATE_UNUSED || e->state == NDP_STATE_INCOMPLETE) {
                            e->state = NDP_STATE_STALE;
                            e->timer_ms = 0;
                        }
                        e->ttl_ms = t->reachable_time_ms * 4;
                        e->probes_sent = 0;
                        ndp_flush_pending(ifx, e);
                    }
                }
            }
        }

        const uint8_t* reply_mac = has_slla ? slla : src_mac;
        (void)ndp_send_na_on(ifx, src_ip, self->ip, ns.target, reply_mac, my_mac, 1);
        return;
    }

    if (h->type == ICMPV6_NEIGHBOR_ADVERT) {
        if (icmp_len < sizeof(icmpv6_na_t)) return;

        icmpv6_na_t na;
        if (!netpkt_copyout(pkt, 0, &na, sizeof(na))) return;
        if (ipv6_is_multicast(na.target)) return;

        uint32_t f = bswap32(na.flags);
        bool router = ((f >> 31) & 1u) != 0;
        bool solicited = ((f >> 30) & 1u) != 0;
        bool override = ((f >> 29) & 1u) != 0;
        if (solicited && ipv6_is_multicast(dst_ip)) return;

        uint8_t tlla[6];
        bool has_tlla = false;
        uint32_t opt_off = (uint32_t)sizeof(icmpv6_na_t);
        uint32_t opt_len = icmp_len - opt_off;
        if (!ndp_read_lladdr_option(pkt, opt_off, opt_len, NDP_OPT_TARGET_LLADDR, tlla, &has_tlla)) return;
        if (has_tlla && !mac_is_unicast(tlla)) has_tlla = false;

        l2_interface_t* l2 = l2_interface_find_by_index(ifx);
        if (!l2) return;

        for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[i];
            if (!v6) continue;
            if (ipv6_cmp(v6->ip, na.target) != 0) continue;

            if (v6->dad_state == IPV6_DAD_IN_PROGRESS || v6->dad_requested) {
                v6->dad_state = IPV6_DAD_FAILED;
                v6->dad_requested = 0;
                v6->dad_timer_ms = 0;
                v6->dad_probes_sent = 0;
                return;
            }
        }

        if (ipv6_is_unspecified(src_ip)) return;
        for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[i];
            if (!v6) continue;
            if (v6->cfg == IPV6_CFG_DISABLE) continue;
            if (ipv6_cmp(v6->ip, src_ip) == 0 || ipv6_cmp(v6->ip, na.target) == 0) return;
        }

        ndp_table_impl_t* t = (ndp_table_impl_t*)l2->nd_table;
        if (!t) return;

        int idx = ndp_find_slot(t, na.target);
        if (idx < 0) return;

        ndp_entry_t* e = &t->entries[idx];
        bool was_router = e->is_router != 0;

        if (e->static_entry) {
            e->is_router = router ? 1 : 0;
        } else if (e->state == NDP_STATE_INCOMPLETE) {
            if (!has_tlla) return;
            mac_copy(e->mac, tlla);
            e->ttl_ms = t->reachable_time_ms * 4;
            e->is_router = router;
            e->probes_sent = 0;
            ndp_mark_neighbor_observed(t, e, solicited);
            ndp_flush_pending(ifx, e);
        } else {
            bool mac_changed = has_tlla && !mac_equal(e->mac, tlla);
            if (!override && mac_changed) {
                if (e->state == NDP_STATE_REACHABLE) {
                    e->state = NDP_STATE_STALE;
                    e->timer_ms = 0;
                }
                return;
            }

            if (mac_changed) {
                mac_copy(e->mac, tlla);
                e->ttl_ms = t->reachable_time_ms * 4;
            }

            if (solicited) ndp_mark_neighbor_observed(t, e, true);
            else if (mac_changed) ndp_mark_neighbor_observed(t, e, false);

            e->is_router = router;
            e->probes_sent = 0;
        }

        bool routers_changed = false;
        for (int r = 0; r < NDP_DEFAULT_ROUTER_MAX; r++) {
            ndp_default_router_t* def = &t->routers[r];
            if (!def->used || ipv6_cmp(def->ip, na.target) != 0) continue;

            if (router && solicited && def->failed) {
                def->failed = 0;
                routers_changed = true;
            } else if (was_router && !router) {
                memset(def, 0, sizeof(*def));
                routers_changed = true;
            }
            break;
        }
        if (routers_changed) ndp_sync_default_routes(ifx, t);
        return;
    }

    if (h->type == ICMPV6_ROUTER_ADVERT) {
        if (icmp_len < sizeof(icmpv6_ra_t)) return;
        if (!ipv6_is_linklocal(src_ip)) return;

        icmpv6_ra_t ra;
        if (!netpkt_copyout(pkt, 0, &ra, sizeof(ra))) return;

        uint16_t router_lifetime = bswap16(ra.router_lifetime);
        uint32_t reachable_time = bswap32(ra.reachable_time);
        uint32_t retrans_timer = bswap32(ra.retrans_timer);

        uint32_t opt_off = sizeof(icmpv6_ra_t);
        uint32_t opt_len = icmp_len - sizeof(icmpv6_ra_t);
        uint8_t slla[6];
        bool has_slla = false;
        if (!ndp_read_lladdr_option(pkt, opt_off, opt_len, NDP_OPT_SOURCE_LLADDR, slla, &has_slla)) return;
        if (has_slla && !mac_is_unicast(slla)) has_slla = false;

        l2_interface_t* l2 = l2_interface_find_by_index(ifx);
        ndp_table_impl_t* t = l2 ? (ndp_table_impl_t*)l2->nd_table : NULL;
        if (!t) return;

        if (ra.cur_hop_limit) l2->ipv6_default_hop_limit = ra.cur_hop_limit;

        int neighbor = ndp_find_slot(t, src_ip);
        if (neighbor < 0 && has_slla) neighbor = ndp_find_free(t);
        if (neighbor < 0 && has_slla) neighbor = ndp_find_replacement(t);
        if (neighbor >= 0) {
            ndp_entry_t* e = &t->entries[neighbor];
            bool new_entry = e->state == NDP_STATE_UNUSED || ipv6_cmp(e->ip, src_ip) != 0;
            if (!new_entry && e->static_entry) e->is_router = 1;
            else {
                if (new_entry) {
                    if (e->state != NDP_STATE_UNUSED) ndp_entry_clear(e);
                    ipv6_cpy(e->ip, src_ip);
                }
                if (has_slla) {
                    bool changed = new_entry || e->state == NDP_STATE_INCOMPLETE || !mac_equal(e->mac, slla);
                    mac_copy(e->mac, slla);
                    e->ttl_ms = t->reachable_time_ms * 4;
                    if (changed) {
                        e->state = NDP_STATE_STALE;
                        e->timer_ms = 0;
                    }
                    ndp_flush_pending(ifx, e);
                }
                e->is_router = 1;
                e->probes_sent = 0;
            }
        }

        int router_slot = -1;
        int free_router = -1;
        int replacement = -1;
        int8_t lowest_pref = 2;
        uint32_t lowest_lifetime = UINT32_MAX;

        for (int i = 0; i < NDP_DEFAULT_ROUTER_MAX; i++) {
            ndp_default_router_t* r = &t->routers[i];
            if (!r->used) {
                if (free_router < 0) free_router = i;
                continue;
            }
            if (ipv6_cmp(r->ip, src_ip) == 0) {
                router_slot = i;
                break;
            }
            if (r->preference < lowest_pref ||
                (r->preference == lowest_pref && r->lifetime_ms < lowest_lifetime)) {
                lowest_pref = r->preference;
                lowest_lifetime = r->lifetime_ms;
                replacement = i;
            }
        }

        if (!router_lifetime) {
            if (router_slot >= 0) memset(&t->routers[router_slot], 0, sizeof(t->routers[router_slot]));
        } else {
            if (router_slot < 0) router_slot = free_router >= 0 ? free_router : replacement;
            if (router_slot >= 0) {
                ndp_default_router_t* r = &t->routers[router_slot];
                memset(r, 0, sizeof(*r));
                r->used = 1;
                ipv6_cpy(r->ip, src_ip);
                r->lifetime_ms = (uint32_t)router_lifetime * 1000;

                switch ((ra.flags >> 3) & 0x03) {
                    case 1:
                        r->preference = 1;
                        break;
                    case 3:
                        r->preference = -1;
                        break;
                    default:
                        break;
                }
            }
        }
        ndp_sync_default_routes(ifx, t);

        if (reachable_time && reachable_time <= NDP_MAX_RA_REACHABLE_TIME_MS) {
            if (t->base_reachable_time_ms != reachable_time) {
                t->base_reachable_time_ms = reachable_time;
                uint32_t low = reachable_time / 2;
                uint32_t high = reachable_time + reachable_time / 2;
                t->reachable_time_ms = rng_between32(&g_rng, low, high + 1);
                if (!t->reachable_time_ms) t->reachable_time_ms = 1;
                t->reachable_recalc_ms = NDP_REACHABLE_RECALC_MS;
            }
        }
        if (retrans_timer) t->retrans_timer_ms = retrans_timer;

        uint8_t rs_index = ifx - 1;
        g_rs_tries[rs_index] = 3;
        g_rs_timer_ms[rs_index] = 0;

        while (opt_len >= 2) {
            uint8_t opt_head[2];
            if (!netpkt_copyout(pkt, opt_off, opt_head, sizeof(opt_head))) break;
            uint8_t opt_type = opt_head[0];
            uint8_t opt_units = opt_head[1];
            if (opt_units == 0) break;

            uint32_t opt_size = (uint32_t)opt_units * 8;
            if (opt_size > opt_len) break;

            if (opt_type == NDP_OPT_PREFIX_INFO && opt_size == sizeof(ndp_opt_prefix_info_t)) {
                ndp_opt_prefix_info_t pio;
                if (!netpkt_copyout(pkt, opt_off, &pio, sizeof(pio))) break;

                uint8_t pfx_len = pio.prefix_length;
                bool autonomous = (pio.flags & 0x40) != 0;
                uint32_t valid_lft = bswap32(pio.valid_lifetime);
                uint32_t pref_lft = bswap32(pio.preferred_lifetime);

                uint8_t pfx[16];
                ipv6_cpy(pfx, pio.prefix);

                if (pref_lft <= valid_lft && pfx_len) ndp_on_ra(ifx, pfx, pfx_len, valid_lft, pref_lft, autonomous, ra.flags);
            } else if (opt_type == NDP_OPT_MTU && opt_size == sizeof(ndp_opt_mtu_t)) {
                uint32_t advertised_mtu = 0;
                if (!netpkt_copyout(pkt, opt_off + 4, &advertised_mtu, sizeof(advertised_mtu))) break;
                advertised_mtu = bswap32(advertised_mtu);

                if (advertised_mtu <= UINT16_MAX) l2_ipv6_set_link_mtu(ifx, (uint16_t)advertised_mtu);
            } else if (opt_type == NDP_OPT_RDNSS && opt_size >= 24 && ((opt_size - 8) % 16) == 0) {
                uint32_t addr_count = (opt_size - 8) / 16;
                uint8_t dns0[16];
                uint8_t dns1[16] = {0};
                if (!netpkt_copyout(pkt, opt_off + 8, dns0, sizeof(dns0))) break;
                if (addr_count > 1 && !netpkt_copyout(pkt, opt_off + 24, dns1, sizeof(dns1))) break;

                l3_ipv6_interface_t* dns_l3 = NULL;
                l3_ipv6_interface_t* fallback = NULL;
                for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
                    l3_ipv6_interface_t* v6 = l2->l3_v6[i];
                    if (!v6 || v6->cfg == IPV6_CFG_DISABLE) continue;
                    if (!(v6->kind & IPV6_ADDRK_GLOBAL)) continue;
                    if (!(v6->cfg & (IPV6_CFG_SLAAC | IPV6_CFG_DHCPV6))) continue;
                    if (!fallback) fallback = v6;

                    if (!ipv6_is_unspecified(v6->prefix)) {
                        if (ipv6_common_prefix_len(v6->prefix, src_ip) < 64) continue;
                    } else if (!ipv6_is_placeholder_gua(v6->ip)) {
                        if (ipv6_is_unspecified(v6->ip) || ipv6_is_multicast(v6->ip) || ipv6_is_linklocal(v6->ip)) continue;
                        if (ipv6_common_prefix_len(v6->ip, src_ip) < 64) continue;
                    }

                    dns_l3 = v6;
                    break;
                }
                if (!dns_l3) dns_l3 = fallback;

                if (dns_l3) {
                    ipv6_cpy(dns_l3->runtime_opts_v6.dns[0], dns0);
                    if (addr_count > 1) ipv6_cpy(dns_l3->runtime_opts_v6.dns[1], dns1);
                    else memset(dns_l3->runtime_opts_v6.dns[1], 0, 16);
                }
            }

            opt_off += opt_size;
            opt_len -= opt_size;
        }

        return;
    }
}

int ndp_daemon_entry(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    rng_init_random(&g_rng);

    const uint32_t tick_ms = 1000;

    while (1) {
        uint8_t n = l2_interface_count();
        for (uint8_t i = 0; i < n; i++) {
            l2_interface_t* l2 = l2_interface_at(i);
            if (l2 && l2->is_up) ndp_table_tick_for_l2(l2->ifindex, tick_ms);
        }

        uint32_t now_ms = get_time();

        for (uint8_t i = 0; i < n; i++) {
            l2_interface_t* l2 = l2_interface_at(i);
            if (!l2) continue;

            if (!l2->is_up) {
                if (l2->ifindex && l2->ifindex <= MAX_L2_INTERFACES) {
                    g_rs_tries[l2->ifindex - 1] = 0;
                    g_rs_timer_ms[l2->ifindex - 1] = 0;
                }
                continue;
            }

            int is_v6_local = 0;

            for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
                l3_ipv6_interface_t* v6 = l2->l3_v6[i];
                if (v6 && v6->is_localhost) {
                    is_v6_local = 1;
                    break;
                }
            }

            if (!is_v6_local) apply_ra_policy(now_ms, l2);
            int has_lla_ok = 0;
            ndp_table_impl_t* ndp = (ndp_table_impl_t*)l2->nd_table;
            uint32_t dad_retrans_ms = ndp ? ndp->retrans_timer_ms : NDP_DEFAULT_RETRANS_TIMER_MS;

            for (int s = 0; s < MAX_IPV6_PER_INTERFACE; s++) {
                l3_ipv6_interface_t* v6 = l2->l3_v6[s];
                if (!v6) continue;
                if (v6->cfg == IPV6_CFG_DISABLE) continue;
                if (ipv6_is_unspecified(v6->ip) || ipv6_is_multicast(v6->ip)) continue;

                if (v6->dad_state == IPV6_DAD_FAILED) {
                    handle_dad_failed(v6);
                    continue;
                }

                if (v6->dad_requested && v6->dad_state == IPV6_DAD_NONE) {
                    if (ipv6_is_unspecified(v6->ip) || ipv6_is_multicast(v6->ip) || ipv6_is_placeholder_gua(v6->ip)) {
                        v6->dad_requested = 0;
                        continue;
                    }

                    v6->dad_requested = 0;
                    v6->dad_state = IPV6_DAD_IN_PROGRESS;
                    v6->dad_probes_sent = 0;
                    v6->dad_timer_ms = 0;

                }

                if (v6->dad_state == IPV6_DAD_IN_PROGRESS) {
                    v6->dad_timer_ms += tick_ms;

                    if (v6->dad_probes_sent < NDP_MAX_PROBES) {
                        if (v6->dad_timer_ms >= dad_retrans_ms) {
                            v6->dad_timer_ms = 0;

                            ndp_send_ns_on(l2->ifindex, v6->ip, (const uint8_t[16]){0}, NULL);
                            v6->dad_probes_sent++;
                        }
                    } else {
                        if (v6->dad_timer_ms >= dad_retrans_ms) {
                            v6->dad_timer_ms = 0;
                            v6->dad_state = IPV6_DAD_OK;

                            uint8_t all_nodes[16];
                            ipv6_make_multicast(2, IPV6_MCAST_ALL_NODES, (const uint8_t[16]){0}, all_nodes);

                            const uint8_t* my_mac = network_get_mac(l2->ifindex);
                            if (my_mac) (void)ndp_send_na_on(l2->ifindex, all_nodes, v6->ip, v6->ip, 0, my_mac, 0);
                            if (ipv6_is_linklocal(v6->ip)) mld_resend_memberships(l2->ifindex);
                        }
                    }
                }

                if (v6->dad_state == IPV6_DAD_OK && ipv6_is_linklocal(v6->ip)) has_lla_ok = 1;
                handle_lifetimes(now_ms, v6);
            }

            if (!has_lla_ok && l2->ifindex && l2->ifindex <= MAX_L2_INTERFACES) {
                g_rs_tries[l2->ifindex - 1] = 0;
                g_rs_timer_ms[l2->ifindex - 1] = 0;
            }

            if (has_lla_ok && l2->ifindex && l2->ifindex <= MAX_L2_INTERFACES) {
                uint8_t idx = (uint8_t)(l2->ifindex - 1);

                if (g_rs_tries[idx] == 0) {
                    ndp_send_rs_on(l2->ifindex);
                    g_rs_tries[idx] = 1;
                    g_rs_timer_ms[idx] = 0;
                } else if (g_rs_tries[idx] < 3) {
                    g_rs_timer_ms[idx] += tick_ms;

                    if (g_rs_timer_ms[idx] >= 4000) {
                        g_rs_timer_ms[idx] = 0;
                        ndp_send_rs_on(l2->ifindex);
                        g_rs_tries[idx]++;
                    }
                }
            }
        }

        msleep(tick_ms);
    }
}