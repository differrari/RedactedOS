#include "ipv4_route.h"
#include "std/memory.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "syscalls/syscalls.h"

struct ipv4_rt_table {
    l3_id_t owner_l3_id;
    uint32_t epoch;
    ipv4_rt_entry_t e[IPV4_RT_PER_IF_MAX];
    int len;
};

static void ipv4_rt_bump(ipv4_rt_table_t* t) {
    if (!t) return;
    t->epoch++;
    if (!t->epoch) t->epoch = 1;
}

bool ipv4_tx_plan_valid(const ipv4_tx_plan_t* plan) {
    if (!plan || !plan->l3_id) return false;

    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(plan->l3_id);
    if (!ipv4_l3_is_ready(v4)) return false;
    if (v4->epoch != plan->l3_epoch) return false;
    return v4->ip == plan->src_ip;
}

bool ipv4_tx_plan_onlink(const ipv4_tx_plan_t* plan, uint32_t dst) {
    if (!ipv4_tx_plan_valid(plan)) return false;
    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(plan->l3_id); 
    if (ipv4_is_loopback(dst)) return v4->is_localhost;
    if (ipv4_is_multicast(dst) || ipv4_is_limited_broadcast(dst) || dst == v4->broadcast) return true;
    return v4->mask && (dst & v4->mask) == (v4->ip & v4->mask);
}

bool ipv4_build_tx_plan(uint32_t dst, const ip_tx_opts_t* hint, ipv4_tx_plan_t* out) {
    if (!out) return false;
    out->l3_id = 0;
    out->l3_epoch = 0;
    out->src_ip = 0;

    if (hint && hint->scope == IP_TX_BOUND_L3) {
        l3_id_t id = hint->index;
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(id);
        if (!ipv4_l3_is_ready(v4) || ipv4_is_loopback(dst) != v4->is_localhost) return false;
        out->l3_id = id;
        out->l3_epoch = v4->epoch;
        out->src_ip = v4->ip;
        return true;
    }

    uint8_t ifindex = 0;
    if (hint && hint->scope == IP_TX_BOUND_L2) {
        l2_interface_t* l2 = l2_interface_find_by_index((uint8_t)hint->index);
        if (!l2 || !l2->is_up) return false;
        ifindex = l2->ifindex;
    }

    l3_id_t chosen = 0;
    if (!ipv4_rt_pick_best_l3(dst, ifindex, &chosen)) {
        uint8_t first = ifindex ? ifindex : 1;
        uint8_t last = ifindex ? ifindex : MAX_L2_INTERFACES;
        for (uint8_t ix = first; ix <= last && !chosen; ix++) {
            l2_interface_t* l2 = l2_interface_find_by_index(ix);
            if (!l2 || !l2->is_up) continue;
            for (int slot = 0; slot < MAX_IPV4_PER_INTERFACE; slot++) {
                l3_ipv4_interface_t* v4 = l2->l3_v4[slot];
                if (!ipv4_l3_is_ready(v4) || ipv4_is_loopback(dst) != v4->is_localhost) continue;
                chosen = v4->l3_id;
                break;
            }
        }
        if (!chosen) return false;
    }

    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen);
    if (!ipv4_l3_is_ready(v4) || ipv4_is_loopback(dst) != v4->is_localhost) return false;

    out->l3_id = chosen;
    out->l3_epoch = v4->epoch;
    out->src_ip = v4->ip;
    return true;
}


ipv4_rt_table_t* ipv4_rt_create(l3_id_t owner_l3_id) {
    ipv4_rt_table_t* t = (ipv4_rt_table_t*)zalloc(sizeof(ipv4_rt_table_t));
    if (!t) return 0;
    t->owner_l3_id = owner_l3_id;
    t->epoch = 1;
    return t;
}

void ipv4_rt_destroy(ipv4_rt_table_t* t) {
    if (!t) return;
    release(t);
}

void ipv4_rt_clear(ipv4_rt_table_t* t) {
    if (!t) return;
    bool changed = t->len != 0;
    t->len = 0;
    memset(t->e, 0, sizeof(t->e));
    if (changed) ipv4_rt_bump(t);
}

bool ipv4_rt_add_in(ipv4_rt_table_t* t, uint32_t network, uint32_t mask, uint32_t gateway, uint16_t metric) {
    if (!t) return false;
    for (int i = 0; i < t->len; i++) {
        if (t->e[i].network == network && t->e[i].mask == mask) {
            if (t->e[i].gateway == gateway && t->e[i].metric == metric) return true;
            t->e[i].gateway = gateway;
            t->e[i].metric = metric;
            ipv4_rt_bump(t);
            return true;
        }
    }
    if (t->len >= IPV4_RT_PER_IF_MAX) return false;
    t->e[t->len++] = (ipv4_rt_entry_t){ network, mask, gateway, metric };
    ipv4_rt_bump(t);
    return true;
}

bool ipv4_rt_del_in(ipv4_rt_table_t* t, uint32_t network, uint32_t mask) {
    if (!t) return false;
    for (int i = 0; i < t->len; i++) {
        if (t->e[i].network == network && t->e[i].mask == mask) {
            t->e[i] = t->e[--t->len];
            memset(&t->e[t->len], 0, sizeof(t->e[0]));
            ipv4_rt_bump(t);
            return true;
        }
    }
    return false;
}

int ipv4_rt_count(const ipv4_rt_table_t* t) {
    return t ? t->len : 0;
}

bool ipv4_rt_get(const ipv4_rt_table_t* t, int index, ipv4_rt_entry_t* out) {
    if (!t || !out || index < 0 || index >= t->len) return false;
    *out = t->e[index];
    return true;
}

uint32_t ipv4_rt_epoch(const ipv4_rt_table_t* t) {
    return t ? t->epoch : 0;
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
            int pl = ipv4_prefix_len(mask);
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

bool ipv4_rt_pick_best_l3(uint32_t dst, uint8_t ifindex, l3_id_t* out_l3){
    int best_pl = -1;
    int best_cost = 0x7FFFFFFF;
    l3_id_t best_l3 = 0;
    uint8_t first = ifindex ? ifindex : 1;
    uint8_t last = ifindex ? ifindex : MAX_L2_INTERFACES;

    for (uint8_t ix = first; ix <= last; ix++) {
        l2_interface_t* l2 = l2_interface_find_by_index(ix);
        if (!l2 || !l2->is_up) continue;

        for (int slot = 0; slot < MAX_IPV4_PER_INTERFACE; slot++) {
            l3_ipv4_interface_t* x = l2->l3_v4[slot];
            if (!ipv4_l3_is_ready(x) || ipv4_is_loopback(dst) != x->is_localhost) continue; 

            int pl_conn = -1;
            if (x->mask){
                uint32_t netx = x->ip & x->mask;
                if ((dst & x->mask) == netx) pl_conn = ipv4_prefix_len(x->mask);
            }

            int pl_tab = -1;
            int met_tab = 0x7FFF;
            if (x->routing_table){
                const ipv4_rt_table_t* rt = (const ipv4_rt_table_t*)x->routing_table;
                if (rt->owner_l3_id && rt->owner_l3_id != x->l3_id) continue;
                int out_pl = -1;
                int out_met = 0x7FFF;
                uint32_t nh;
                if (ipv4_rt_lookup_in(rt, dst, &nh, &out_pl, &out_met)) {
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