#include "ipv4_route.h"
#include "std/memory.h"

extern uintptr_t malloc(uint64_t size);
extern void free(void* ptr, uint64_t size);

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
    free(t, sizeof(*t));
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
