#include "ipv6_route.h"
#include "std/memory.h"
#include "std/string.h"
#include "net/internet_layer/ipv6_utils.h"
#include "networking/interface_manager.h"
#include "syscalls/syscalls.h"

struct ipv6_rt_table {
    ipv6_rt_entry_t e[IPV6_RT_PER_IF_MAX];
    int len;
};

ipv6_rt_table_t* ipv6_rt_create(void) {
    ipv6_rt_table_t* t = malloc(sizeof(*t));
    if (!t) return 0;

    memset(t, 0, sizeof(*t));
    return t;
}

void ipv6_rt_destroy(ipv6_rt_table_t* t) {
    if (!t) return;

    free(t, sizeof(*t));
}

void ipv6_rt_clear(ipv6_rt_table_t* t) {
    if (!t) return;

    t->len = 0;
    memset(t->e, 0, sizeof(t->e));
}

bool ipv6_rt_add_in(ipv6_rt_table_t* t, const uint8_t net[16], uint8_t plen, const uint8_t gw[16], uint16_t metric) {
    if (!t) return false;

    for (int i = 0; i < t->len; i++) {
        if (t->e[i].prefix_len == plen && memcmp(t->e[i].network, net, 16) == 0) {
            memcpy(t->e[i].gateway, gw, 16);
            t->e[i].metric = metric;
            return true;
        }
    }

    if (t->len >= IPV6_RT_PER_IF_MAX) return false;

    memcpy(t->e[t->len].network, net, 16);
    memcpy(t->e[t->len].gateway, gw, 16);
    t->e[t->len].prefix_len = plen;
    t->e[t->len].metric = metric;
    t->len++;

    return true;
}

bool ipv6_rt_del_in(ipv6_rt_table_t* t, const uint8_t net[16], uint8_t plen) {
    if (!t) return false;

    for (int i = 0; i < t->len; i++) {
        if (t->e[i].prefix_len == plen && memcmp(t->e[i].network, net, 16) == 0) {
            t->e[i] = t->e[--t->len];
            memset(&t->e[t->len], 0, sizeof(t->e[0]));
            return true;
        }
    }

    return false;
}

bool ipv6_rt_lookup_in(const ipv6_rt_table_t* t, const uint8_t dst[16], uint8_t next_hop[16], int* out_pl, int* out_metric) {
    if (!t) return false;

    int best_pl = -1;
    int best_metric = 0x7FFF;
    uint8_t best_gw[16] = {0};

    for (int i = 0; i < t->len; i++) {
        bool match = false;

        if (t->e[i].prefix_len == 0) {
            match = true;
        } else {
            int plen = t->e[i].prefix_len;
            int fb = plen / 8;
            int rb = plen % 8;

            match = true;
            for (int j = 0; j < fb; j++) {
                if (dst[j] != t->e[i].network[j]) {
                    match = false;
                    break;
                }
            }

            if (match && rb) {
                uint8_t m = (uint8_t)(0xFF << (8 - rb));
                if ((dst[fb] & m) != (t->e[i].network[fb] & m)) match = false;
            }
        }

        if (!match) continue;

        int pl = t->e[i].prefix_len;
        int met = t->e[i].metric;

        if (pl > best_pl || (pl == best_pl && met < best_metric)) {
            best_pl = pl;
            best_metric = met;
            memcpy(best_gw, t->e[i].gateway, 16);
        }
    }

    if (best_pl < 0) return false;

    if (next_hop) memcpy(next_hop, best_gw, 16);
    if (out_pl) *out_pl =best_pl;
    if (out_metric) *out_metric = best_metric;

    return true;
}

void ipv6_rt_ensure_basics(ipv6_rt_table_t* t, const uint8_t ip[16], uint8_t plen, const uint8_t gw[16], uint16_t base_metric) {
    if (!t) return;

    if (ip && plen &&!ipv6_is_unspecified(ip)) {
        uint8_t net[16];
        ipv6_cpy(net, ip);

        if (plen < 128) {
            int fb = plen / 8;
            int rb = plen % 8;

            for (int i = fb + (rb > 0); i < 16; i++) net[i] = 0;

            if (rb) {
                uint8_t m = (uint8_t)(0xFF <<(8 - rb));
                net[fb] &=m;
            }
        }

        ipv6_rt_add_in(t, net, plen, (const uint8_t[16]){0}, base_metric);
    }

    if (gw && !ipv6_is_unspecified(gw)) {
        uint8_t z[16] = {0};
        ipv6_rt_add_in(t, z, 0, gw, (uint16_t)(base_metric + 1));
    }
}

void ipv6_rt_sync_basics(ipv6_rt_table_t* t, const uint8_t ip[16], uint8_t plen, const uint8_t gw[16], uint16_t base_metric) {
    if (!t) return;

    uint8_t z[16] = {0};

    if (gw && !ipv6_is_unspecified(gw)) ipv6_rt_add_in(t, z, 0,gw, (uint16_t)(base_metric + 1));
    else ipv6_rt_del_in(t, z, 0);

    if (ip && plen && !ipv6_is_unspecified(ip)) {
        uint8_t net[16];
        ipv6_cpy(net, ip);

        if (plen < 128) {
            int fb = plen / 8;
            int rb = plen % 8;

            for (int i = fb + (rb > 0); i < 16; i++)net[i] = 0;

            if (rb) {
                uint8_t m = (uint8_t)(0xFF << (8 - rb));
                net[fb] &= m;
            }
        }
        
        ipv6_rt_add_in(t, net, plen, (const uint8_t[16]) {0}, base_metric);
    }
}

bool ipv6_rt_pick_best_l3_in(const uint8_t* l3_ids, int n_ids, const uint8_t dst[16], uint8_t* out_l3) {
    int best_pl = -1;
    int best_cost = 0x7FFFFFFF;
    uint8_t best_l3 = 0;

    for (int i = 0; i < n_ids; i++) {
        l3_ipv6_interface_t* x = l3_ipv6_find_by_id(l3_ids[i]);
        if (!x || !x->l2)continue;
        if (x->cfg == IPV6_CFG_DISABLE) continue;
        if (ipv6_is_unspecified(x->ip)) continue;

        int l2base = x->l2->base_metric;

        int pl_conn = -1;
        if (x->prefix_len) {
            int pl = ipv6_common_prefix_len(dst, x->ip);
            if (pl >= x->prefix_len) pl_conn = x->prefix_len;
        }

        int pl_tab = -1;
        int met_tab = 0x7FFF;

        if (x->routing_table) {
            uint8_t via[16] = {0};
            int out_pl = -1;
            int out_met = 0x7FFF;

            if (ipv6_rt_lookup_in((const ipv6_rt_table_t*)x->routing_table, dst, via, &out_pl, &out_met)) {
                pl_tab = out_pl;
                met_tab = out_met;
            }
        }

        int cand_pl = pl_conn;
        int cand_cost = l2base;

        if (pl_tab > cand_pl || (pl_tab == cand_pl && l2base + met_tab < cand_cost)) {
            cand_pl = pl_tab;
            cand_cost = l2base + met_tab;
        }

        if (cand_pl > best_pl || (cand_pl == best_pl && cand_cost <best_cost)) {
            best_pl = cand_pl;
            best_cost = cand_cost;
            best_l3 = l3_ids[i];
        }
    }

    if (best_pl < 0) return false;

    if (out_l3) *out_l3 = best_l3;
    return true;
}