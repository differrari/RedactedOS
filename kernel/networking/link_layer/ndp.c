#include "ndp.h"
#include "networking/internet_layer/icmpv6.h"
#include "std/memory.h"
#include "std/string.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/internet_layer/ipv6_route.h"
#include "net/checksums.h"
#include "syscalls/syscalls.h"
#include "networking/network.h"
#include "process/scheduler.h"
#include "math/rng.h"

typedef struct {
    ndp_entry_t entries[NDP_TABLE_MAX];
    uint8_t init;
} ndp_table_impl_t;

static uint32_t g_ndp_reachable_time_ms = 30000;
static uint32_t g_ndp_retrans_timer_ms = 1000;
static uint8_t g_ndp_max_probes = 3;
static volatile uint16_t g_ndp_pid = 0xFFFF;

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
    uint8_t zero16[16] = {0};

    make_random_iid(iid);

    if (ipv6_is_linklocal(v6->ip)) {
        new_ip[0] = 0xFE;
        new_ip[1] = 0x80;
        memset(new_ip + 2, 0, 6);
        memcpy(new_ip + 8, iid, 8);

        (void)l3_ipv6_update(v6->l3_id, new_ip, 64, zero16, v6->cfg, v6->kind);
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

    if (memcmp(v6->prefix, zero16, 16) != 0) ipv6_cpy(new_ip, v6->prefix);
    else {
        ipv6_cpy(new_ip, v6->ip);
        memset(new_ip + 8, 0, 8);
    }

    memcpy(new_ip + 8, iid, 8);

    (void)l3_ipv6_update(v6->l3_id, new_ip, 64, v6->gateway, v6->cfg, v6->kind);
    (void)ndp_request_dad_on(v6->l2 ? v6->l2->ifindex : 0, new_ip);
}

static void handle_lifetimes(uint32_t now_ms, l3_ipv6_interface_t* v6) {
    if (!v6) return;
    if (ipv6_is_placeholder_gua(v6->ip)) return;
    if (ipv6_is_unspecified(v6->ip)) return;
    if (ipv6_is_linklocal(v6->ip)) return;
    if (!v6->ra_last_update_ms) return;

    uint32_t elapsed_ms = now_ms >= v6->ra_last_update_ms ? now_ms - v6->ra_last_update_ms : 0;

    if (v6->preferred_lifetime && v6->preferred_lifetime != 0xFFFFFFFFu) {
        uint64_t pref_ms = (uint64_t)v6->preferred_lifetime * 1000ull;
        if ((uint64_t)elapsed_ms >= pref_ms) v6->preferred_lifetime = 0;
    }

    if (v6->valid_lifetime == 0xFFFFFFFFu) return;

    uint64_t valid_ms = (uint64_t)v6->valid_lifetime * 1000ull;
    if ((uint64_t)elapsed_ms >= valid_ms) {
        if (!l3_ipv6_remove_from_interface(v6->l3_id)) (void)l3_ipv6_set_enabled(v6->l3_id, false);
    }
}

static void apply_ra_policy(uint32_t now_ms, l2_interface_t* l2) {
    if (!l2) return;

    uint8_t ifx = l2->ifindex;
    if (!ifx || ifx > MAX_L2_INTERFACES) return;

    int has_lla_ok = 0;
    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (!v6) continue;
        if (v6->cfg == IPV6_CFG_DISABLE) continue;
        if (!ipv6_is_linklocal(v6->ip)) continue;
        if (v6->dad_state == IPV6_DAD_OK) {
            has_lla_ok = 1;
            break;
        }
    }

    if (!has_lla_ok) return;

    uint8_t zero16[16] = {0};

    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (!v6) continue;
        if (v6->cfg == IPV6_CFG_DISABLE) continue;
        if (!(v6->kind & IPV6_ADDRK_GLOBAL)) continue;
        if (!(v6->cfg & (IPV6_CFG_SLAAC | IPV6_CFG_DHCPV6))) continue;
        if (!v6->ra_has)continue;
        if (memcmp(v6->prefix, zero16, 16) == 0) continue;
        uint8_t m = (v6->ra_flags & RA_FLAG_M) ? 1u : 0u;
        uint8_t o = (v6->ra_flags & RA_FLAG_O) ? 1u : 0u;
        if (!v6->ra_autonomous) {
            if (m) {
                uint8_t gw[16];

                if (v6->ra_is_default) ipv6_cpy(gw, v6->gateway);
                else memset(gw, 0, 16);

                v6->dhcpv6_stateless = 0;
                v6->dhcpv6_stateless_done = 0;

                if (v6->cfg != IPV6_CFG_DHCPV6 || ipv6_is_placeholder_gua(v6->ip)) {
                    uint8_t z[16] = {0};
                    (void)l3_ipv6_update(v6->l3_id, z, 0, gw, IPV6_CFG_DHCPV6, v6->kind);
                } else {
                    (void)l3_ipv6_update(v6->l3_id, v6->ip, v6->prefix_len, gw, IPV6_CFG_DHCPV6, v6->kind);
                }
            } else {
                v6->dhcpv6_stateless = o ? 1 : 0;
                v6->dhcpv6_stateless_done = 0;
            }

            continue;
        }
        v6->dhcpv6_stateless = o ? 1 : 0;
        v6->dhcpv6_stateless_done = 0;

        if (v6->cfg != IPV6_CFG_SLAAC) {
            uint8_t ph[16];
            uint8_t gw[16];

            ipv6_make_placeholder_gua(ph);

            if (v6->ra_is_default) ipv6_cpy(gw, v6->gateway);
            else memset(gw, 0, 16);

            (void)l3_ipv6_update(v6->l3_id, ph, 64, gw, IPV6_CFG_SLAAC, v6->kind);
        }

        if (ipv6_is_placeholder_gua(v6->ip)) {
            uint8_t iid[8];
            uint8_t ip[16];

            make_random_iid(iid);
            ipv6_cpy(ip, v6->prefix);
            memcpy(ip + 8, iid, 8);

            (void)l3_ipv6_update(v6->l3_id, ip, 64, v6->gateway, IPV6_CFG_SLAAC, v6->kind);

            v6->timestamp_created = now_ms;
            memcpy(v6->interface_id, ip + 8, 8);

            if (v6->dad_state == IPV6_DAD_NONE && !v6->dad_requested) (void)ndp_request_dad_on(ifx, ip);
            continue;
        }

        uint8_t gw[16];
        if (v6->ra_is_default) ipv6_cpy(gw, v6->gateway);
        else memset(gw, 0, 16);

        (void)l3_ipv6_update(v6->l3_id, v6->ip, v6->prefix_len, gw, IPV6_CFG_SLAAC, v6->kind);

        v6->timestamp_created = now_ms;
        memcpy(v6->interface_id, v6->ip + 8, 8);
    }
}

static void ndp_on_ra(uint8_t ifindex, const uint8_t router_ip[16], uint16_t router_lifetime, const uint8_t prefix[16], uint8_t prefix_len, uint32_t valid_lft, uint32_t preferred_lft, uint8_t autonomous, uint8_t ra_flags) {
    if (!ifindex) return;
    if (prefix_len != 64) return;
    if (ipv6_is_unspecified(prefix) || ipv6_is_multicast(prefix) || ipv6_is_linklocal(prefix)) return;

    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return;

    uint32_t now_ms = get_time();
    uint8_t zero16[16] = {0};
    l3_ipv6_interface_t* slot = NULL;

    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (!v6) continue;
        if (v6->cfg == IPV6_CFG_DISABLE) continue;
        if (!(v6->kind == IPV6_ADDRK_GLOBAL)) continue;
        if (!(v6->cfg & (IPV6_CFG_SLAAC | IPV6_CFG_DHCPV6))) continue;

        if (memcmp(v6->prefix, zero16, 16) != 0) {
            if (ipv6_common_prefix_len(v6->prefix, prefix) >=64) {
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

        uint8_t id = l3_ipv6_add_to_interface(ifindex, ph, 64, zero16, IPV6_CFG_SLAAC, IPV6_ADDRK_GLOBAL);
        if (!id) return;

        slot = l3_ipv6_find_by_id(id);
        if (!slot) return;
    }

    slot->ra_has = 1;
    slot->ra_autonomous = autonomous ? 1 : 0;
    slot->ra_is_default = router_lifetime != 0;
    slot->ra_last_update_ms = now_ms;
    slot->ra_flags = ra_flags;

    ipv6_cpy(slot->prefix, prefix);

    if (slot->ra_is_default && router_ip) ipv6_cpy(slot->gateway, router_ip);
    else ipv6_cpy(slot->gateway, zero16);

    slot->valid_lifetime = valid_lft;
    slot->preferred_lifetime = preferred_lft;

    if (memcmp(slot->ip, zero16, 16) == 0) slot->timestamp_created = now_ms;

    if(!ipv6_is_placeholder_gua(slot->ip) && !ipv6_is_unspecified(slot->ip)) memcpy(slot->interface_id, slot->ip + 8, 8);
}

ndp_table_t* ndp_table_create(void) {
    ndp_table_impl_t* t = (ndp_table_impl_t*)malloc(sizeof(ndp_table_impl_t));
    if (!t) return 0;

    memset(t, 0, sizeof(*t));
    t->init = 1;

    return (ndp_table_t*)t;
}

void ndp_table_destroy(ndp_table_t* t) {
    if (!t) return;
    free_sized(t, sizeof(ndp_table_impl_t));
}

static ndp_table_impl_t* l2_ndp(uint8_t ifindex) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return 0;
    if (!l2->nd_table) l2->nd_table = ndp_table_create();
    return (ndp_table_impl_t*)l2->nd_table;
}

static int ndp_find_slot(ndp_table_impl_t* t, const uint8_t ip[16]) {
    if (!t) return -1;

    for (int i = 0; i < NDP_TABLE_MAX; i++) {
        if (!t->entries[i].ttl_ms) continue;
        if (memcmp(t->entries[i].ip, ip, 16) == 0) return i;
    }

    return -1;
}

static int ndp_find_free(ndp_table_impl_t* t) {
    if (!t) return -1;

    for (int i = 0; i < NDP_TABLE_MAX; i++) if (t->entries[i].ttl_ms == 0 && t->entries[i].state == NDP_STATE_UNUSED) return i;

    return -1;
}

static void ndp_entry_clear(ndp_entry_t* e) {
    memset(e, 0, sizeof(*e));
    e->state = NDP_STATE_UNUSED;
}

void ndp_table_put_for_l2(uint8_t ifindex, const uint8_t ip[16], const uint8_t mac[6], uint32_t ttl_ms, bool router) {
    ndp_table_impl_t* t = l2_ndp(ifindex);
    if (!t) return;

    int idx = ndp_find_slot(t, ip);
    if (idx < 0) idx = ndp_find_free(t);

    if (idx < 0) {
        uint32_t best_ttl = 0xFFFFFFFFu;
        int best_i = -1;

        for (int i = 0; i < NDP_TABLE_MAX; i++) {
            ndp_entry_t* e = &t->entries[i];
            if (e->state == NDP_STATE_UNUSED || e->ttl_ms == 0) {
                best_i = i;
                break;
            }

            if (e->is_router && e->router_lifetime_ms) continue;

            if (e->ttl_ms < best_ttl) {
                best_ttl = e->ttl_ms;
                best_i = i;
            }
        }

        if (best_i < 0) best_i = 0;
        idx = best_i;
    }

    ndp_entry_t* e = &t->entries[idx];
    memcpy(e->ip, ip, 16);

    if (mac) {
        memcpy(e->mac, mac, 6);
        e->state = NDP_STATE_REACHABLE;
        e->timer_ms = g_ndp_reachable_time_ms;
    }

    if (ttl_ms == 0) {
        ttl_ms = g_ndp_reachable_time_ms * 4;
        if (ttl_ms == 0) ttl_ms = 1;
    }

    e->ttl_ms = ttl_ms;
    e->is_router = router ? 1 : 0;
    e->router_lifetime_ms = router ? ttl_ms : 0;
    e->probes_sent = 0;
}

static bool ndp_table_get_for_l2(uint8_t ifindex, const uint8_t ip[16], uint8_t mac_out[6]) {
    ndp_table_impl_t* t = l2_ndp(ifindex);
    if (!t) return false;

    for (int i = 0; i < NDP_TABLE_MAX; i++) {
        ndp_entry_t* e = &t->entries[i];
        if (!e->ttl_ms) continue;
        if (e->state == NDP_STATE_UNUSED) continue;
        if (e->state == NDP_STATE_INCOMPLETE) continue;
        if (memcmp(e->ip, ip, 16) != 0) continue;

        memcpy(mac_out, e->mac, 6);
        return true;
    }

    return false;
}

static bool ndp_send_na_on(uint8_t ifindex, const uint8_t dst_ip[16], const uint8_t src_ip[16], const uint8_t target_ip[16], const uint8_t dst_mac_in[6], const uint8_t my_mac[6], uint8_t solicited) {
    uint32_t plen = (uint32_t)(sizeof(icmpv6_na_t) + sizeof(icmpv6_opt_lladdr_t));
    uintptr_t buf = (uintptr_t)malloc(plen);
    if (!buf) return false;

    icmpv6_na_t* na = (icmpv6_na_t*)buf;
    na->hdr.type = 136;
    na->hdr.code = 0;
    na->hdr.checksum = 0;

    uint32_t flags = 0;
    if (solicited) flags |= (1u << 30);
    flags |= (1u << 29);
    na->flags = bswap32(flags);

    memcpy(na->target, target_ip, 16);

    icmpv6_opt_lladdr_t* opt = (icmpv6_opt_lladdr_t*)(buf + sizeof(icmpv6_na_t));
    opt->type = 2;
    opt->length = 1;
    memcpy(opt->mac, my_mac, 6);

    na->hdr.checksum = bswap16(checksum16_pipv6(src_ip, dst_ip, 58, (const uint8_t*)buf, plen));

    uint8_t dst_mac[6];
    if (ipv6_is_multicast(dst_ip)) ipv6_multicast_mac(dst_ip, dst_mac);
    else memcpy(dst_mac, dst_mac_in, 6);

    bool ok = icmpv6_send_on_l2(ifindex, dst_ip, src_ip, dst_mac, (const void*)buf, plen, 255);

    free_sized((void*)buf, plen);
    return ok;
}

static void ndp_send_ns_on(uint8_t ifindex, const uint8_t target_ip[16], const uint8_t src_ip[16]) {
    bool dad = ipv6_is_unspecified(src_ip);
    uint32_t plen = (uint32_t)sizeof(icmpv6_ns_t) + (dad ? 0u : (uint32_t)sizeof(icmpv6_opt_lladdr_t));
    uintptr_t buf = (uintptr_t)malloc(plen);
    if (!buf) return;

    icmpv6_ns_t* ns = (icmpv6_ns_t*)buf;
    ns->hdr.type = 135;
    ns->hdr.code = 0;
    ns->hdr.checksum = 0;
    ns->rsv = 0;

    memcpy(ns->target, target_ip, 16);

    if (!dad) {
        icmpv6_opt_lladdr_t* opt = (icmpv6_opt_lladdr_t*)(buf + sizeof(icmpv6_ns_t));
        opt->type = 1;
        opt->length = 1;

        const uint8_t* mac = network_get_mac(ifindex);
        if (mac) memcpy(opt->mac, mac, 6);
        else memset(opt->mac, 0, 6);
    }

    uint8_t dst_ip[16];
    ipv6_make_multicast(2, IPV6_MCAST_SOLICITED_NODE, target_ip, dst_ip);

    ns->hdr.checksum = bswap16(checksum16_pipv6(src_ip, dst_ip, 58, (const uint8_t*)buf, plen));

    uint8_t dst_mac[6];
    ipv6_multicast_mac(dst_ip, dst_mac);

    icmpv6_send_on_l2(ifindex, dst_ip, src_ip, dst_mac, (const void*)buf, plen, 255);
    free_sized((void*)buf, plen);
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

    uint32_t plen = (uint32_t)(sizeof(icmpv6_rs_t) + sizeof(icmpv6_opt_lladdr_t));
    uintptr_t buf = (uintptr_t)malloc(plen);
    if (!buf) return;

    icmpv6_rs_t* rs = (icmpv6_rs_t*)buf;
    rs->hdr.type = 133;
    rs->hdr.code = 0;
    rs->hdr.checksum = 0;
    rs->reserved =0;

    icmpv6_opt_lladdr_t* opt = (icmpv6_opt_lladdr_t*)(buf + sizeof(icmpv6_rs_t));
    opt->type = 1;
    opt->length = 1;

    const uint8_t* mac = network_get_mac(ifindex);
    if (mac) memcpy(opt->mac, mac, 6);
    else memset(opt->mac, 0, 6);

    rs->hdr.checksum = bswap16(checksum16_pipv6(src_ip, dst_ip, 58, (const uint8_t*)buf, plen));

    uint8_t dst_mac[6];
    ipv6_multicast_mac(dst_ip, dst_mac);

    icmpv6_send_on_l2(ifindex, dst_ip, src_ip, dst_mac, (const void*)buf, plen, 255);
    free_sized((void*)buf, plen);
}

static void ndp_send_probe(uint8_t ifindex, ndp_entry_t* e) {
    uint8_t src_ip[16] = {0};
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);

    if (l2) {
        for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[i];
            if (!v6) continue;
            if (v6->cfg == IPV6_CFG_DISABLE) continue;
            if (v6->dad_state!= IPV6_DAD_OK) continue;

            if (ipv6_is_linklocal(v6->ip)) {
                memcpy(src_ip, v6->ip, 16);
                break;
            }

            if (ipv6_is_unspecified(src_ip) && !ipv6_is_unspecified(v6->ip))
                memcpy(src_ip, v6->ip, 16);
        }
    }

    ndp_send_ns_on(ifindex, e->ip, src_ip);
}

static void ndp_table_tick_for_l2(uint8_t ifindex, uint32_t ms) {
    ndp_table_impl_t* t = l2_ndp(ifindex);
    if (!t) return;

    for (int i = 0; i < NDP_TABLE_MAX; i++) {
        ndp_entry_t* e =&t->entries[i];

        if (!e->ttl_ms) {
            if (e->state != NDP_STATE_UNUSED) ndp_entry_clear(e);
            continue;
        }

        if (e->ttl_ms <= ms) {
            ndp_entry_clear(e);
            continue;
        }

        e->ttl_ms -= ms;

        if (e->is_router && e->router_lifetime_ms) {
            if (e->router_lifetime_ms <= ms) {
                e->is_router = 0;
                e->router_lifetime_ms = 0;
            } else {
                e->router_lifetime_ms -= ms;
            }
        }

        if (e->timer_ms) {
            if (e->timer_ms <= ms)e->timer_ms = 0;
            else e->timer_ms -= ms;
        }

        switch (e->state) {
        case NDP_STATE_INCOMPLETE:
            if (e->timer_ms == 0) {
                if (e->probes_sent < g_ndp_max_probes) {
                    e->probes_sent++;
                    e->timer_ms = g_ndp_retrans_timer_ms;
                    ndp_send_probe(ifindex, e);
                } else {
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
                e->timer_ms = g_ndp_retrans_timer_ms;
                ndp_send_probe(ifindex, e);
            }
            break;

        case NDP_STATE_PROBE:
            if (e->timer_ms == 0) {
                if (e->probes_sent < g_ndp_max_probes) {
                    e->probes_sent++;
                    e->timer_ms = g_ndp_retrans_timer_ms;
                    ndp_send_probe(ifindex, e);
                } else ndp_entry_clear(e);
            }
            break;

        default:
            break;
        }
    }
}

static void ndp_tick_all(uint32_t ms) {
    uint8_t n = l2_interface_count();

    for (uint8_t i = 0; i < n; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2) continue;
        if (!l2->is_up) continue;

        ndp_table_tick_for_l2(l2->ifindex, ms);
    }
}

bool ndp_resolve_on(uint16_t ifindex, const uint8_t next_hop[16], uint8_t out_mac[6], uint32_t timeout_ms) {
    if (ipv6_is_multicast(next_hop)) {
        ipv6_multicast_mac(next_hop, out_mac);
        return true;
    }

    if (ndp_table_get_for_l2((uint8_t)ifindex, next_hop, out_mac)) return true;

    ndp_table_impl_t* t = l2_ndp((uint8_t)ifindex);
    if (t) {
        int idx = ndp_find_slot(t, next_hop);
        if (idx >= 0) {
            ndp_entry_t* e = &t->entries[idx];
            if (e->ttl_ms && e->is_router && e->state != NDP_STATE_UNUSED && e->state != NDP_STATE_INCOMPLETE) {
                memcpy(out_mac, e->mac, 6);
                return true;
            }
        }
    }

    uint8_t src_ip[16] = {0};
    l2_interface_t* l2 = l2_interface_find_by_index((uint8_t)ifindex);

    if (l2) {
        for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[i];
            if (!v6) continue;
            if (v6->cfg == IPV6_CFG_DISABLE) continue;
            if (v6->dad_state != IPV6_DAD_OK) continue;

            if (ipv6_is_linklocal(v6->ip)) {
                memcpy(src_ip, v6->ip, 16);
                break;
            }

            if (ipv6_is_unspecified(src_ip) && !ipv6_is_unspecified(v6->ip))
                memcpy(src_ip, v6->ip, 16);
        }
    }

    t = l2_ndp((uint8_t)ifindex);
    if (t) {
        int idx = ndp_find_slot(t, next_hop);
        if (idx < 0) idx = ndp_find_free(t);

        if (idx >= 0) {
            ndp_entry_t* e = &t->entries[idx];
            memcpy(e->ip, next_hop, 16);
            memset(e->mac, 0, 6);
            e->ttl_ms = g_ndp_reachable_time_ms * 4;
            e->is_router = 0;
            e->router_lifetime_ms = 0;
            e->state = NDP_STATE_INCOMPLETE;
            e->timer_ms = g_ndp_retrans_timer_ms;
            e->probes_sent = 0;
        }
    }

    ndp_send_ns_on((uint8_t)ifindex, next_hop, src_ip);

    uint32_t waited = 0;
    const uint32_t poll = 50;

    while (waited < timeout_ms) {
        ndp_table_tick_for_l2((uint8_t)ifindex, poll);
        if (ndp_table_get_for_l2((uint8_t)ifindex, next_hop, out_mac)) return true;
        msleep(poll);
        waited += poll;
    }

    return false;
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

        uint8_t sn[16];
        ipv6_make_multicast(2, IPV6_MCAST_SOLICITED_NODE, v6->ip, sn);
        (void)l2_ipv6_mcast_join(ifindex, sn);

        return true;
    }

    return false;
}

void ndp_input(uint16_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], const uint8_t src_mac[6], const uint8_t* icmp, uint32_t icmp_len) {
    if (!ifindex || !src_ip || !dst_ip || !icmp || icmp_len < sizeof(icmpv6_hdr_t)) return;

    const icmpv6_hdr_t* h = (const icmpv6_hdr_t*)icmp;
    if (h->code != 0) return;

    if (h->type == 135) {
        if (icmp_len < sizeof(icmpv6_ns_t)) return;

        const icmpv6_ns_t* ns = (const icmpv6_ns_t*)icmp;
        if (ipv6_is_multicast(ns->target)) return;

        l2_interface_t* l2 = l2_interface_find_by_index((uint8_t)ifindex);
        if (!l2) return;

        l3_ipv6_interface_t* self = 0;

        for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[i];
            if (!v6) continue;
            if (v6->cfg == IPV6_CFG_DISABLE) continue;

            if (ipv6_cmp(v6->ip, ns->target) == 0) {
                self = v6;
                break;
            }
        }

        if (!self) return;

        if (ipv6_is_unspecified(src_ip)) {
            if (self->dad_state == IPV6_DAD_IN_PROGRESS || self->dad_requested) {
                self->dad_state = IPV6_DAD_FAILED;
                self->dad_timer_ms = 0;
                self->dad_probes_sent = 0;
                self->dad_requested = 0;
            }
            return;
        }

        if (self->dad_state != IPV6_DAD_OK) return;

        ndp_table_put_for_l2((uint8_t)ifindex, src_ip, src_mac, 180000, false);

        uint8_t src_my[16] = {0};

        for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[i];
            if (!v6) continue;
            if (v6->cfg == IPV6_CFG_DISABLE) continue;
            if (v6->dad_state != IPV6_DAD_OK) continue;

            if (ipv6_cmp(v6->ip, ns->target) == 0) {
                ipv6_cpy(src_my, v6->ip);
                break;
            }
        }

        if (ipv6_is_unspecified(src_my)) return;

        const uint8_t* my_mac = network_get_mac((uint8_t)ifindex);
        if (!my_mac) return;

        ndp_send_na_on((uint8_t)ifindex, src_ip, src_my, ns->target, src_mac, my_mac, 1);
        return;
    }

    if (h->type == 136) {
        if (icmp_len < sizeof(icmpv6_na_t)) return;

        const icmpv6_na_t* na = (const icmpv6_na_t*)icmp;
        if (ipv6_is_multicast(na->target)) return;

        l2_interface_t* l2 = l2_interface_find_by_index((uint8_t)ifindex);

        if (l2) {
            for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
                l3_ipv6_interface_t* v6 = l2->l3_v6[i];
                if (!v6) continue;
                if (ipv6_cmp(v6->ip, na->target) != 0) continue;

                if (v6->dad_state == IPV6_DAD_IN_PROGRESS || v6->dad_requested) {
                    v6->dad_state = IPV6_DAD_FAILED;
                    v6->dad_requested = 0;
                    v6->dad_timer_ms = 0;
                    v6->dad_probes_sent = 0;
                    return;
                }
            }
        }

        if (ipv6_is_unspecified(src_ip)) return;

        uint32_t f = bswap32(na->flags);
        uint8_t router = (uint8_t)((f >> 31) & 1u);
        uint8_t solicited = (uint8_t)((f >> 30) & 1u);
        uint8_t override = (uint8_t)((f >> 29) & 1u);

        ndp_table_impl_t* t = l2_ndp((uint8_t)ifindex);
        if (!t) return;

        int idx = ndp_find_slot(t, na->target);
        if (idx < 0) idx = ndp_find_free(t);

        if (idx < 0) {
            uint32_t best_ttl = 0xFFFFFFFFu;
            int best_i = -1;

            for (int i = 0; i < NDP_TABLE_MAX; i++) {
                ndp_entry_t* e = &t->entries[i];
                if (e->state == NDP_STATE_UNUSED || e->ttl_ms == 0) {
                    best_i = i;
                    break;
                }

                if (e->is_router && e->router_lifetime_ms) continue;

                if (e->ttl_ms < best_ttl) {
                    best_ttl = e->ttl_ms;
                    best_i = i;
                }
            }

            if (best_i < 0) best_i = 0;
            idx = best_i;
        }

        ndp_entry_t* e = &t->entries[idx];

        uint8_t old_mac[6];
        memcpy(old_mac, e->mac, 6);

        if (e->ttl_ms == 0 && e->state == NDP_STATE_UNUSED) {
            memcpy(e->ip, na->target, 16);
            memcpy(e->mac, src_mac, 6);
            e->ttl_ms = g_ndp_reachable_time_ms * 4;
            e->probes_sent = 0;
            e->is_router = router ? 1 : 0;
            e->router_lifetime_ms = e->is_router ? e->ttl_ms : 0;

            if (solicited) {
                e->state = NDP_STATE_REACHABLE;
                e->timer_ms = g_ndp_reachable_time_ms;
            } else {
                e->state = NDP_STATE_STALE;
                e->timer_ms = 0;
            }
        } else {
            int mac_changed = memcmp(old_mac, src_mac, 6) != 0;

            if (e->state == NDP_STATE_INCOMPLETE) {
                memcpy(e->mac, src_mac, 6);
                e->ttl_ms = g_ndp_reachable_time_ms * 4;

                if (solicited) {
                    e->state = NDP_STATE_REACHABLE;
                    e->timer_ms = g_ndp_reachable_time_ms;
                } else {
                    e->state = NDP_STATE_STALE;
                    e->timer_ms = 0;
                }
            } else {
                if (!mac_changed) {
                    if (solicited) {
                        e->state = NDP_STATE_REACHABLE;
                        e->timer_ms = g_ndp_reachable_time_ms;
                    }
                } else {
                    if (override) {
                        memcpy(e->mac, src_mac, 6);
                        e->ttl_ms = g_ndp_reachable_time_ms * 4;

                        if (solicited) {
                            e->state = NDP_STATE_REACHABLE;
                            e->timer_ms = g_ndp_reachable_time_ms;
                        } else {
                            e->state = NDP_STATE_STALE;
                            e->timer_ms = 0;
                        }
                    } else {
                        e->state = NDP_STATE_STALE;
                        e->timer_ms = 0;
                    }
                }
            }

            if (router) e->is_router = 1;
            if (!e->is_router) e->router_lifetime_ms = 0;
            if (e->is_router && !e->router_lifetime_ms) e->router_lifetime_ms = e->ttl_ms;
        }

        e->probes_sent = 0;
        return;
    }

    if (h->type == 134) {
        if (icmp_len < sizeof(icmpv6_ra_t)) return;

        const icmpv6_ra_t* ra = (const icmpv6_ra_t*)icmp;

        uint16_t router_lifetime = bswap16(ra->router_lifetime);
        uint32_t reachable_time = bswap32(ra->reachable_time);
        uint32_t retrans_timer = bswap32(ra->retrans_timer);

        uint32_t router_lifetime_ms = (uint32_t)router_lifetime * 1000u;

        if (router_lifetime == 0) ndp_table_put_for_l2((uint8_t)ifindex, src_ip, src_mac, 180000, false);
        else ndp_table_put_for_l2((uint8_t)ifindex, src_ip, src_mac, router_lifetime_ms, true);

        if (reachable_time) g_ndp_reachable_time_ms = reachable_time;
        if (retrans_timer) g_ndp_retrans_timer_ms = retrans_timer;

        const uint8_t* opt = (const uint8_t*)(ra + 1);
        uint32_t opt_len = icmp_len - (uint32_t)sizeof(icmpv6_ra_t);

        uint8_t idx = (uint8_t)(ifindex - 1);

        if (idx < MAX_L2_INTERFACES) {
            g_rs_tries[idx] = 3;
            g_rs_timer_ms[idx] = 0;
        }

        while (opt_len >= 2) {
            uint8_t opt_type = opt[0];
            uint8_t opt_units = opt[1];
            if (opt_units == 0) break;

            uint32_t opt_size = (uint32_t)opt_units * 8u;
            if (opt_size > opt_len) break;

            if (opt_type == 3&&opt_size >= (uint32_t)sizeof(ndp_opt_prefix_info_t)) {
                const ndp_opt_prefix_info_t* pio = (const ndp_opt_prefix_info_t*)opt;

                uint8_t pfx_len = pio->prefix_length;
                uint8_t autonomous = (pio->flags & 0x40u) ? 1u : 0u;
                uint32_t valid_lft = bswap32(pio->valid_lifetime);
                uint32_t pref_lft = bswap32(pio->preferred_lifetime);

                uint8_t pfx[16];
                memcpy(pfx, pio->prefix, 16);

                if (pfx_len != 0) ndp_on_ra((uint8_t)ifindex, src_ip, router_lifetime, pfx, pfx_len, valid_lft, pref_lft, autonomous, ra->flags);
            } else if (opt_type == 5 && opt_size >= (uint32_t)sizeof(ndp_opt_mtu_t)) {
                uint32_t mtu32 = 0;
                memcpy(&mtu32, opt + 4, 4);
                mtu32= bswap32(mtu32);

                if (mtu32 >= 1280u && mtu32 <= 65535u) {
                    l2_interface_t* l2 = l2_interface_find_by_index((uint8_t)ifindex);
                    if (l2) {
                        for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
                            l3_ipv6_interface_t* v6 = l2->l3_v6[i];
                            if (!v6) continue;
                            if (v6->cfg == IPV6_CFG_DISABLE) continue;
                            if (v6->mtu < 1280) continue;
                            v6->mtu = mtu32;
                        }
                    }
                }
            } else if (opt_type == 25 && opt_size >= 24u) {
                l2_interface_t* l2 = l2_interface_find_by_index((uint8_t)ifindex);
                if (l2) {
                    uint32_t addr_bytes = opt_size - 8u;
                    uint32_t addr_count = addr_bytes / 16u;

                    uint8_t zero16[16] = {0};

                    const uint8_t* a0 = (addr_count >= 1) ? (opt + 8) : zero16;
                    const uint8_t* a1 = (addr_count >= 2) ? (opt + 24) : zero16;

                    l3_ipv6_interface_t* slot = NULL;

                    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
                        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
                        if (!v6) continue;
                        if (v6->cfg == IPV6_CFG_DISABLE) continue;
                        if (!(v6->kind & IPV6_ADDRK_GLOBAL)) continue;
                        if (!(v6->cfg & (IPV6_CFG_SLAAC | IPV6_CFG_DHCPV6))) continue;

                        if (memcmp(v6->prefix, zero16, 16) != 0) {
                            if (ipv6_common_prefix_len(v6->prefix, src_ip) >= 64) {
                                slot = v6;
                                break;
                            }
                        } else {
                            if (ipv6_is_placeholder_gua(v6->ip)) {
                                slot = v6;
                                break;
                            }
                            if (!ipv6_is_unspecified(v6->ip) && !ipv6_is_multicast(v6->ip) && !ipv6_is_linklocal(v6->ip)) {
                                if (ipv6_common_prefix_len(v6->ip, src_ip) >= 64) {
                                    slot = v6;
                                    break;
                                }
                            }
                        }
                    }

                    if (!slot) {
                        for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
                            l3_ipv6_interface_t* v6 = l2->l3_v6[i];
                            if (!v6) continue;
                            if (v6->cfg == IPV6_CFG_DISABLE) continue;
                            if (!(v6->kind & IPV6_ADDRK_GLOBAL)) continue;
                            if (!(v6->cfg & (IPV6_CFG_SLAAC | IPV6_CFG_DHCPV6))) continue;
                            slot = v6;
                            break;
                        }
                    }

                    if (slot) {
                        if (addr_count >= 1) memcpy(slot->runtime_opts_v6.dns[0], a0, 16);
                        else memset(slot->runtime_opts_v6.dns[0], 0, 16);

                        if (addr_count >= 2) memcpy(slot->runtime_opts_v6.dns[1], a1, 16);
                        else memset(slot->runtime_opts_v6.dns[1], 0, 16);
                    }
                }
            }

            opt += opt_size;
            opt_len -= opt_size;
        }

        return;
    }
}

int ndp_daemon_entry(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    g_ndp_pid = (uint16_t)get_current_proc_pid();
    uint64_t virt_timer;
    asm volatile ("mrs %0, cntvct_el0" : "=r"(virt_timer));
    rng_seed(&g_rng, virt_timer);

    const uint32_t tick_ms = 1000;

    while (1) {
        ndp_tick_all(tick_ms);

        uint32_t now_ms = get_time();
        uint8_t n = l2_interface_count();

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

                    uint8_t sn[16];
                    ipv6_make_multicast(2, IPV6_MCAST_SOLICITED_NODE, v6->ip, sn);
                    (void)l2_ipv6_mcast_join(l2->ifindex, sn);
                }

                if (v6->dad_state == IPV6_DAD_IN_PROGRESS) {
                    v6->dad_timer_ms += tick_ms;

                    if (v6->dad_probes_sent < g_ndp_max_probes) {
                        if (v6->dad_timer_ms >= 1000) {
                            v6->dad_timer_ms = 0;

                            uint8_t sn[16];
                            uint8_t zero16[16] = {0};

                            ipv6_make_multicast(2, IPV6_MCAST_SOLICITED_NODE, v6->ip, sn);
                            (void)l2_ipv6_mcast_join(l2->ifindex, sn);

                            ndp_send_ns_on(l2->ifindex, v6->ip, zero16);
                            v6->dad_probes_sent++;
                        }
                    } else {
                        if (v6->dad_timer_ms >= 1000) {
                            v6->dad_timer_ms = 0;
                            v6->dad_state = IPV6_DAD_OK;

                            uint8_t all_nodes[16];
                            uint8_t zero16[16] = {0};
                            ipv6_make_multicast(2, IPV6_MCAST_ALL_NODES, zero16, all_nodes);

                            const uint8_t* my_mac = network_get_mac(l2->ifindex);
                            if (my_mac) (void)ndp_send_na_on(l2->ifindex, all_nodes, v6->ip, v6->ip, 0, my_mac, 0);
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