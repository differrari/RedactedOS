#include "ipv6_route.h"
#include "std/memory.h"
#include "std/string.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/interface_manager.h"
#include "networking/link_layer/ndp.h"
#include "syscalls/syscalls.h"

#define IPV6_REDIRECT_MAX 32

struct ipv6_rt_table {
    l3_id_t owner_l3_id;
    uint32_t epoch;
    ipv6_rt_entry_t e[IPV6_RT_PER_IF_MAX];
    int len;
};

typedef struct {
    uint8_t used;
    uint8_t ifindex;
    l3_id_t l3_id;
    uint32_t l3_epoch;
    uint32_t route_epoch;
    uint8_t dst[16];
    uint8_t target[16];
} ipv6_redirect_entry_t;

static ipv6_redirect_entry_t g_redirects[IPV6_REDIRECT_MAX] = {0};
static uint8_t g_redirect_rr = 0;

static void ipv6_rt_bump(ipv6_rt_table_t* t) {
    if (!t) return;
    t->epoch++;
    if (!t->epoch) t->epoch = 1;
}

static bool v6_l3_ok_for_tx(l3_ipv6_interface_t* v6, int dst_is_ll, int dst_is_loop) {
    if (!ipv6_l3_is_ready(v6)) return false;
    if (v6->is_localhost && !dst_is_loop) return false;

    int src_is_ll = ipv6_is_linklocal(v6->ip) ? 1 : 0;
    if (src_is_ll != dst_is_ll) return false;
    return true;
}

bool ipv6_tx_plan_valid(const ipv6_tx_plan_t* plan) {
    if (!plan || !plan->l3_id) return false;

    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(plan->l3_id);
    if (!ipv6_l3_is_ready(v6)) return false;
    if (v6->epoch != plan->l3_epoch) return false;
    return ipv6_cmp(v6->ip, plan->src_ip) == 0;
}

bool ipv6_tx_plan_onlink(const ipv6_tx_plan_t* plan, const uint8_t dst[16]) {
    if (!dst || !ipv6_tx_plan_valid(plan)) return false;
    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(plan->l3_id); 
    if (ipv6_is_loopback(dst)) return v6->is_localhost;
    if (ipv6_is_linklocal(dst) || ipv6_is_linkscope_mcast(dst) || ipv6_is_multicast(dst)) return true;
    if (!(v6->ra_has && (v6->cfg & IPV6_CFG_SLAAC)) && v6->prefix_len && ipv6_common_prefix_len(v6->ip, dst) >= v6->prefix_len) return true;
    if (v6->routing_table) {
        uint8_t via[16] = {0};
        if (ipv6_rt_lookup_in((const ipv6_rt_table_t*)v6->routing_table, dst, via, NULL, NULL) && ipv6_is_unspecified(via)) return true;
    }
    return false;
}

bool ipv6_build_tx_plan(const uint8_t dst[16], const ip_tx_opts_t* hint, ipv6_tx_plan_t* out) {
    if (!dst || !out) return false;

    memset(out, 0, sizeof(*out));

    int dst_is_ll = (ipv6_is_linklocal(dst) || ipv6_is_linkscope_mcast(dst)) ? 1 : 0;
    int dst_is_loop = ipv6_is_loopback(dst) ? 1 : 0;

    if (hint && hint->scope == IP_TX_BOUND_L3) {
        l3_id_t id = hint->target.l3_id;
        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(id);
        if (!v6_l3_ok_for_tx(v6, dst_is_ll, dst_is_loop)) return false;
        out->l3_id = id;
        out->l3_epoch = v6->epoch;
        ipv6_cpy(out->src_ip, v6->ip);
        return true;
    }

    uint8_t ifindex = 0;
    if (hint && hint->scope == IP_TX_BOUND_L2) {
        l2_interface_t* l2 = l2_interface_find_by_index(hint->target.ifindex);
        if (!l2 || !l2->is_up) return false;
        ifindex = l2->ifindex;
    }

    l3_id_t chosen = 0;
    if (!ipv6_rt_pick_best_l3(dst, ifindex, &chosen)) {
        uint8_t first = ifindex ? ifindex : 1;
        uint8_t last = ifindex ? ifindex : MAX_L2_INTERFACES;
        for (uint8_t ix = first; ix <= last && !chosen; ix++) {
            l2_interface_t* l2 = l2_interface_find_by_index(ix);
            if (!l2 || !l2->is_up) continue;
            for (int slot = 0; slot < MAX_IPV6_PER_INTERFACE; slot++) {
                l3_ipv6_interface_t* v6 = l2->l3_v6[slot];
                if (!v6_l3_ok_for_tx(v6, dst_is_ll, dst_is_loop)) continue;
                chosen = v6->l3_id;
                break;
            }
        }
        if (!chosen) return false;
    }

    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(chosen);
    if (!v6_l3_ok_for_tx(v6, dst_is_ll, dst_is_loop)) return false;

    out->l3_id = chosen;
    out->l3_epoch = v6->epoch;
    ipv6_cpy(out->src_ip, v6->ip);
    return true;
}

ipv6_rt_table_t* ipv6_rt_create(l3_id_t owner_l3_id) {
    ipv6_rt_table_t* t = zalloc(sizeof(*t));
    if (!t) return 0;

    t->owner_l3_id = owner_l3_id;
    t->epoch = 1;
    return t;
}

void ipv6_rt_destroy(ipv6_rt_table_t* t) {
    if (!t) return;

    release(t);
}

void ipv6_rt_clear(ipv6_rt_table_t* t) {
    if (!t) return;
    bool changed = t->len != 0;
    t->len = 0;
    memset(t->e, 0, sizeof(t->e));
    if (changed) ipv6_rt_bump(t);
}

bool ipv6_rt_add_in(ipv6_rt_table_t* t, const uint8_t net[16], uint8_t plen, const uint8_t gw[16], uint16_t metric) {
    if (!t) return false;

    for (int i = 0; i < t->len; i++) {
        if (t->e[i].prefix_len == plen && ipv6_cmp(t->e[i].network, net) == 0) {
            if (ipv6_cmp(t->e[i].gateway, gw) == 0 && t->e[i].metric == metric) return true;
            ipv6_cpy(t->e[i].gateway, gw);
            t->e[i].metric = metric;
            ipv6_rt_bump(t);
            return true;
        }
    }

    if (t->len >= IPV6_RT_PER_IF_MAX) return false;

    ipv6_cpy(t->e[t->len].network, net);
    ipv6_cpy(t->e[t->len].gateway, gw);
    t->e[t->len].prefix_len = plen;
    t->e[t->len].metric = metric;
    t->len++;
    ipv6_rt_bump(t);

    return true;
}

bool ipv6_rt_del_in(ipv6_rt_table_t* t, const uint8_t net[16], uint8_t plen) {
    if (!t) return false;

    for (int i = 0; i < t->len; i++) {
        if (t->e[i].prefix_len == plen && ipv6_cmp(t->e[i].network, net) == 0) {
            t->e[i] = t->e[--t->len];
            memset(&t->e[t->len], 0, sizeof(t->e[0]));
            ipv6_rt_bump(t);
            return true;
        }
    }

    return false;
}

int ipv6_rt_count(const ipv6_rt_table_t* t) {
    return t ? t->len : 0;
}

bool ipv6_rt_get(const ipv6_rt_table_t* t, int index, ipv6_rt_entry_t* out) {
    if (!t || !out || index < 0 || index >= t->len) return false;
    *out = t->e[index];
    return true;
}

uint32_t ipv6_rt_epoch(const ipv6_rt_table_t* t) {
    return t ? t->epoch : 0;
}

bool ipv6_rt_lookup_in(const ipv6_rt_table_t* t, const uint8_t dst[16], uint8_t next_hop[16], int* out_pl, int* out_metric) {
    if (!t) return false;

    int best_pl = -1;
    int best_metric = 0x7FFF;
    uint8_t best_gw[16] = {0};

    for (int i = 0; i < t->len; i++) {
        bool match = false;

        if (t->e[i].prefix_len == 0) match = true;
        else match = ipv6_common_prefix_len(dst, t->e[i].network) >= t->e[i].prefix_len;

        if (!match) continue;

        int pl = t->e[i].prefix_len;
        int met = t->e[i].metric;

        if (pl > best_pl || (pl == best_pl && met < best_metric)) {
            best_pl = pl;
            best_metric = met;
            ipv6_cpy(best_gw, t->e[i].gateway);
        }
    }

    l3_ipv6_interface_t* owner = l3_ipv6_find_by_id(t->owner_l3_id);
    l2_interface_t* l2 = owner ? owner->l2 : NULL;
    if (l2 && !ipv6_is_unspecified(dst) && !ipv6_is_loopback(dst) && !ipv6_is_linklocal(dst) && !ipv6_is_multicast(dst)) {
        int pl = ndp_onlink_prefix_len_for_l2(l2->ifindex, dst);
        int met = l2->base_metric;
        if (pl > best_pl || (pl == best_pl && met < best_metric)) {
            best_pl = pl;
            best_metric = met;
            memset(best_gw, 0, sizeof(best_gw));
        }
    }

    if (best_pl < 0) return false;

    if (next_hop) ipv6_cpy(next_hop, best_gw);
    if (out_pl) *out_pl =best_pl;
    if (out_metric) *out_metric = best_metric;

    return true;
}

bool ipv6_next_hop_for_l3(l3_id_t l3_id, const uint8_t dst[16], uint8_t next_hop[16]) {
    l3_ipv6_interface_t* src_v6 = l3_ipv6_find_by_id(l3_id);
    if (!ipv6_l3_is_ready(src_v6)) return false;
    ipv6_rt_table_t* rt = (ipv6_rt_table_t*)src_v6->routing_table;

    if (rt) {
        uint32_t route_epoch = ipv6_rt_epoch(rt);

        for (int i = 0; i < IPV6_REDIRECT_MAX; i++) {
            ipv6_redirect_entry_t* e = &g_redirects[i];
            if (!e->used || e->l3_id != src_v6->l3_id || ipv6_cmp(e->dst, dst) != 0) continue;
            if (e->l3_epoch != src_v6->epoch || e->route_epoch != route_epoch) {
                memset(e, 0, sizeof(*e));
                continue;
            }

            ipv6_cpy(next_hop, e->target);
            return true;
        }
    }

    uint8_t via[16] = {0};
    if (rt && ipv6_rt_lookup_in(rt, dst, via, NULL, NULL)) {
        if (!ipv6_is_unspecified(via)) ipv6_cpy(next_hop, via);
        else ipv6_cpy(next_hop, dst);
    } else if (!(src_v6->ra_has && (src_v6->cfg & IPV6_CFG_SLAAC)) && src_v6->prefix_len && ipv6_common_prefix_len(dst, src_v6->ip) >= src_v6->prefix_len) ipv6_cpy(next_hop, dst);
    else if (!ipv6_is_unspecified(src_v6->gateway) && ipv6_is_linklocal(src_v6->gateway)) ipv6_cpy(next_hop, src_v6->gateway);
    else return false;

    return true;
}

bool ipv6_redirect_update(l3_id_t l3_id, const uint8_t router[16], const uint8_t dst[16], const uint8_t target[16]) {
    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(l3_id);
    if (!ipv6_l3_is_ready(v6) || !v6->routing_table || !router || !dst || !target) return false;

    uint8_t current_next_hop[16];
    if (!ipv6_next_hop_for_l3(l3_id, dst, current_next_hop) || ipv6_cmp(current_next_hop, router) != 0) return false;

    int slot = -1;
    for (int i = 0; i < IPV6_REDIRECT_MAX; i++) {
        ipv6_redirect_entry_t* e = &g_redirects[i];
        if (e->used && e->l3_id == l3_id && ipv6_cmp(e->dst, dst) == 0) {
            slot = i;
            break;
        }
        if (!e->used && slot < 0) slot = i;
    }

    if (slot < 0) {
        slot = g_redirect_rr;
        g_redirect_rr = (uint8_t)((g_redirect_rr + 1) % IPV6_REDIRECT_MAX);
    }

    ipv6_redirect_entry_t* e = &g_redirects[slot];
    e->used = 0;
    e->ifindex = v6->l2->ifindex;
    e->l3_id = l3_id;
    e->l3_epoch = v6->epoch;
    e->route_epoch = ipv6_rt_epoch((const ipv6_rt_table_t*)v6->routing_table);
    ipv6_cpy(e->dst, dst);
    ipv6_cpy(e->target, target);
    e->used = 1;
    return true;
}

void ipv6_redirect_invalidate(uint8_t ifindex, const uint8_t next_hop[16]) {
    if (!ifindex) return;

    for (int i = 0; i < IPV6_REDIRECT_MAX; i++) {
        ipv6_redirect_entry_t* e = &g_redirects[i];
        if (!e->used || e->ifindex != ifindex) continue;
        if (!next_hop || ipv6_cmp(e->target, next_hop) == 0) memset(e, 0, sizeof(*e));
    }
}

void ipv6_rt_ensure_basics(ipv6_rt_table_t* t, const uint8_t ip[16], uint8_t plen, const uint8_t gw[16], uint16_t base_metric) {
    if (!t) return;

    if (ip && plen && !ipv6_is_unspecified(ip) && !ipv6_is_placeholder_gua(ip)) {
        uint8_t net[16];
        ipv6_prefix_network(ip, plen, net);
        ipv6_rt_add_in(t, net, plen, (const uint8_t[16]){0}, base_metric);
    }

    if (gw && !ipv6_is_unspecified(gw)) {
        ipv6_rt_add_in(t, (const uint8_t[16]){0}, 0, gw, (uint16_t)(base_metric + 1));
    }
}

void ipv6_rt_sync_basics(ipv6_rt_table_t* t, const uint8_t ip[16], uint8_t plen, const uint8_t gw[16], uint16_t base_metric) {
    if (!t) return;

    if (gw && !ipv6_is_unspecified(gw)) ipv6_rt_add_in(t, (const uint8_t[16]){0}, 0,gw, (uint16_t)(base_metric + 1));
    else ipv6_rt_del_in(t, (const uint8_t[16]){0}, 0);

    if (ip && plen && !ipv6_is_unspecified(ip) && !ipv6_is_placeholder_gua(ip)) {
        uint8_t net[16];
        ipv6_prefix_network(ip, plen, net);
        l3_ipv6_interface_t* owner = l3_ipv6_find_by_id(t->owner_l3_id);
        if (owner && owner->ra_has && (owner->cfg & IPV6_CFG_SLAAC)) ipv6_rt_del_in(t, net, plen);
        else ipv6_rt_add_in(t, net, plen, (const uint8_t[16]) {0}, base_metric);
    }
}

bool ipv6_rt_pick_best_l3(const uint8_t dst[16], uint8_t ifindex, l3_id_t* out_l3) {
    if (!dst) return false;

    int best_pl = -1;
    int best_cost = 0x7FFFFFFF;
    l3_id_t best_l3 = 0;
    int dst_is_ll = (ipv6_is_linklocal(dst) || ipv6_is_linkscope_mcast(dst)) ? 1 : 0;
    int dst_is_loop = ipv6_is_loopback(dst) ? 1 : 0;
    uint8_t first = ifindex ? ifindex : 1;
    uint8_t last = ifindex ? ifindex : MAX_L2_INTERFACES;

    for (uint8_t ix = first; ix <= last; ix++) {
        l2_interface_t* l2 = l2_interface_find_by_index(ix);
        if (!l2 || !l2->is_up) continue;

        for (int slot = 0; slot < MAX_IPV6_PER_INTERFACE; slot++) {
            l3_ipv6_interface_t* x = l2->l3_v6[slot];
            if (!v6_l3_ok_for_tx(x, dst_is_ll, dst_is_loop)) continue;

            int pl_conn = -1;
            if (!(x->ra_has && (x->cfg & IPV6_CFG_SLAAC)) && x->prefix_len) {
                int pl = ipv6_common_prefix_len(dst, x->ip);
                if (pl >= x->prefix_len) pl_conn = x->prefix_len;
            }

            int pl_tab = -1;
            int met_tab = 0x7FFF;

            if (x->routing_table) {
                const ipv6_rt_table_t* rt = (const ipv6_rt_table_t*)x->routing_table;
                if (rt->owner_l3_id && rt->owner_l3_id != x->l3_id) continue;
                uint8_t via[16] = {0};
                int out_pl = -1;
                int out_met = 0x7FFF;

                if (ipv6_rt_lookup_in(rt, dst, via, &out_pl, &out_met)) {
                    pl_tab = out_pl;
                    met_tab = out_met;
                }
            }

            int cand_pl = pl_conn;
            int cand_cost = l2->base_metric;

            if (pl_tab > cand_pl || (pl_tab == cand_pl && met_tab < cand_cost)) {
                cand_pl = pl_tab;
                cand_cost = met_tab;
            }

            if (cand_pl > best_pl || (cand_pl == best_pl && cand_cost < best_cost) ||
                (cand_pl == best_pl && cand_cost == best_cost && x->l3_id < best_l3)) {
                best_pl = cand_pl;
                best_cost = cand_cost;
                best_l3 = x->l3_id;
            }
        }
    }

    if (best_pl < 0) return false;
    if (out_l3) *out_l3 = best_l3;
    return true;
}