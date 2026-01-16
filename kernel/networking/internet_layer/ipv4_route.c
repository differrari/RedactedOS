#include "ipv4_route.h"
#include "std/memory.h"
#include "networking/interface_manager.h"
#include "syscalls/syscalls.h"

static bool v4_l3_ok_for_tx(l3_ipv4_interface_t* v4){
    if (!v4 || !v4->l2) return false;
    if (!v4->l2->is_up) return false;
    if (v4->mode == IPV4_CFG_DISABLED) return false;
    if (v4->is_localhost) return false;
    if (!v4->ip) return false;
    if (!v4->port_manager) return false;
    return true;
}

static bool l3_allowed(uint8_t id, const uint8_t* allowed, int n){
    if (!allowed || n <= 0) return true;
    for (int i = 0; i < n; ++i) if (allowed[i] == id) return true;
    return false;
}

bool ipv4_build_tx_plan(uint32_t dst, const ip_tx_opts_t* hint, const uint8_t* allowed_l3, int allowed_n, ipv4_tx_plan_t* out){
    if (!out) return false;
    out->l3_id = 0;
    out->src_ip = 0;
    out->fixed_opts.scope = IP_TX_AUTO;
    out->fixed_opts.index = 0;

    if (hint && hint->scope == IP_TX_BOUND_L3) {
        uint8_t id = hint->index;
        if (!l3_allowed(id, allowed_l3, allowed_n)) return false;
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(id);
        if (!v4_l3_ok_for_tx(v4)) return false;
        out->l3_id = id;
        out->src_ip = v4->ip;
        out->fixed_opts.scope = IP_TX_BOUND_L3;
        out->fixed_opts.index = id;
        return true;
    }

    uint8_t cand[64];
    int n = 0;

    if (hint && hint->scope == IP_TX_BOUND_L2) {
        l2_interface_t* l2 = l2_interface_find_by_index(hint->index);
        if (!l2 || !l2->is_up) return false;
        for (int s = 0; s < MAX_IPV4_PER_INTERFACE && n < (int)sizeof(cand); ++s){
            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
            if (!v4_l3_ok_for_tx(v4)) continue;
            if (!l3_allowed(v4->l3_id, allowed_l3, allowed_n)) continue;
            cand[n++] = v4->l3_id;
        }
    } else {
        uint8_t cnt = l2_interface_count();
        for (uint8_t i = 0; i < cnt && n < (int)sizeof(cand); ++i){
            l2_interface_t* l2 = l2_interface_at(i);
            if (!l2 || !l2->is_up) continue;
            for (int s = 0; s < MAX_IPV4_PER_INTERFACE && n < (int)sizeof(cand); ++s){
                l3_ipv4_interface_t* v4 = l2->l3_v4[s];
                if (!v4_l3_ok_for_tx(v4)) continue;
                if (!l3_allowed(v4->l3_id, allowed_l3, allowed_n)) continue;
                cand[n++] = v4->l3_id;
            }
        }
    }

    if (n == 0) return false;

    uint8_t chosen = 0;
    if (!ipv4_rt_pick_best_l3_in(cand, n, dst, &chosen)) chosen = cand[0];

    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen);
    if (!v4_l3_ok_for_tx(v4)) return false;

    out->l3_id = chosen;
    out->src_ip = v4->ip;
    out->fixed_opts.scope = IP_TX_BOUND_L3;
    out->fixed_opts.index = chosen;
    return true;
}

struct ipv4_rt_table {
    ipv4_rt_entry_t e[IPV4_RT_PER_IF_MAX];
    int len;
};

static int prefix_len(uint32_t m) {
    int n = 0;
    while (m & 0x80000000u) { n++; m <<= 1; }
    return n;
}

ipv4_rt_table_t* ipv4_rt_create(void) {
    ipv4_rt_table_t* t = (ipv4_rt_table_t*)malloc(sizeof(ipv4_rt_table_t));
    if (!t) return 0;
    memset(t, 0, sizeof(*t));
    return t;
}

void ipv4_rt_destroy(ipv4_rt_table_t* t) {
    if (!t) return;
    free_sized(t, sizeof(*t));
}

void ipv4_rt_clear(ipv4_rt_table_t* t) {
    if (!t) return;
    t->len = 0;
    memset(t->e, 0, sizeof(t->e));
}

bool ipv4_rt_add_in(ipv4_rt_table_t* t, uint32_t network, uint32_t mask, uint32_t gateway, uint16_t metric) {
    if (!t) return false;
    for (int i = 0; i < t->len; i++) {
        if (t->e[i].network == network && t->e[i].mask == mask) {
            t->e[i].gateway = gateway;
            t->e[i].metric = metric;
            return true;
        }
    }
    if (t->len >= IPV4_RT_PER_IF_MAX) return false;
    t->e[t->len++] = (ipv4_rt_entry_t){ network, mask, gateway, metric };
    return true;
}

bool ipv4_rt_del_in(ipv4_rt_table_t* t, uint32_t network, uint32_t mask) {
    if (!t) return false;
    for (int i = 0; i < t->len; i++) {
        if (t->e[i].network == network && t->e[i].mask == mask) {
            t->e[i] = t->e[--t->len];
            memset(&t->e[t->len], 0, sizeof(t->e[0]));
            return true;
        }
    }
    return false;
}

bool ipv4_rt_lookup_in(const ipv4_rt_table_t* t, uint32_t dst, uint32_t* next_hop, int* out_prefix_len, int* out_metric) {
    if (!t) return false;
    int best_pl = -1;
    int best_metric = 0x7FFF;
    uint32_t best_nh = 0;

    for (int i = 0; i < t->len; i++) {
        uint32_t net = t->e[i].network;
        uint32_t mask = t->e[i].mask;
        if (mask == 0 || ((dst & mask) == net)) {
            int pl = prefix_len(mask);
            int met = t->e[i].metric;
            if (pl > best_pl || (pl == best_pl && met < best_metric)) {
                best_pl = pl;
                best_metric = met;
                best_nh = t->e[i].gateway ? t->e[i].gateway : dst;
            }
        }
    }

    if (best_pl < 0) return false;
    if (next_hop) *next_hop = best_nh;
    if (out_prefix_len) *out_prefix_len = best_pl;
    if (out_metric) *out_metric = best_metric;
    return true;
}

void ipv4_rt_ensure_basics(ipv4_rt_table_t* t, uint32_t ip, uint32_t mask, uint32_t gw, uint16_t base_metric) {
    if (!t) return;
    if (ip && mask) {
        uint32_t net = ip & mask;
        (void)ipv4_rt_add_in(t, net, mask, 0, base_metric);
    }
    if (gw) {
        (void)ipv4_rt_add_in(t, 0, 0, gw, (uint16_t)(base_metric + 1));
    }
}

void ipv4_rt_sync_basics(ipv4_rt_table_t* t, uint32_t ip, uint32_t mask, uint32_t gw, uint16_t base_metric) {
    if (!t) return;
    if (gw) {
        (void)ipv4_rt_add_in(t, 0, 0, gw, (uint16_t)(base_metric + 1));
    } else {
        (void)ipv4_rt_del_in(t, 0, 0);
    }
    if (ip && mask) {
        uint32_t net = ip & mask;
        (void)ipv4_rt_add_in(t, net, mask, 0, base_metric);
    }
}
bool ipv4_rt_pick_best_l3_in(const uint8_t* l3_ids, int n_ids, uint32_t dst, uint8_t* out_l3){
    int best_pl = -1;
    int best_cost = 0x7FFFFFFF;
    uint8_t best_l3 = 0;
    for (int i=0;i<n_ids;i++){
        l3_ipv4_interface_t* x = l3_ipv4_find_by_id(l3_ids[i]);
        if (!x || !x->l2) continue;
        if (x->mode == IPV4_CFG_DISABLED) continue;
        int l2base = (int)x->l2->base_metric;
        int pl_conn = -1;
        if (x->mask){
            uint32_t netx = x->ip & x->mask;
            if ((dst & x->mask) == netx) pl_conn = prefix_len(x->mask);
        }
        int pl_tab = -1, met_tab = 0x7FFF;
        if (x->routing_table){
            int out_pl = -1, out_met = 0x7FFF;
            uint32_t nh;
            if (ipv4_rt_lookup_in((const ipv4_rt_table_t*)x->routing_table, dst, &nh, &out_pl, &out_met)){
                pl_tab = out_pl;
                met_tab = out_met;
            }
        }
        int cand_pl = pl_conn;
        int cand_cost = l2base;
        if (pl_tab > cand_pl || (pl_tab == cand_pl && (l2base + met_tab) < cand_cost)){
            cand_pl = pl_tab;
            cand_cost = l2base + met_tab;
        }
        if (cand_pl > best_pl || (cand_pl == best_pl && cand_cost < best_cost) || (cand_pl == best_pl && cand_cost == best_cost && l3_ids[i] < best_l3)){
            best_pl = cand_pl;
            best_cost = cand_cost;
            best_l3 = l3_ids[i];
        }
    }
    if (best_pl < 0) return false;
    if (out_l3) *out_l3 = best_l3;
    return true;
}