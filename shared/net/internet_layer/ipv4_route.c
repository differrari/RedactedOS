#include "ipv4_route.h"
#include "std/memfunctions.h"

static ipv4_rt_entry_t g_rt[IPV4_RT_MAX];
static int g_rt_len = 0;

void ipv4_rt_init() {
    g_rt_len = 0;
    memset(g_rt, 0, sizeof(g_rt));
}

bool ipv4_rt_add(uint32_t network, uint32_t mask, uint32_t gateway)
{
    if (g_rt_len >= IPV4_RT_MAX) return false;

    for (int i = 0; i < g_rt_len; ++i) {
        if (g_rt[i].network == network && g_rt[i].mask == mask) {
            g_rt[i].gateway = gateway;
            return true;
        }
    }
    g_rt[g_rt_len++] = (ipv4_rt_entry_t){ network, mask, gateway };
    return true;
}

bool ipv4_rt_del(uint32_t network, uint32_t mask)
{
    for (int i = 0; i < g_rt_len; ++i) {
        if (g_rt[i].network == network && g_rt[i].mask == mask) {
            g_rt[i] = g_rt[--g_rt_len];
            memset(&g_rt[g_rt_len], 0, sizeof(g_rt[0]));
            return true;
        }
    }
    return false;
}

static inline int prefix_len(uint32_t mask)
{
    int len = 0;
    while (mask & 0x80000000U) { ++len; mask <<= 1; }
    return len;
}

bool ipv4_rt_lookup(uint32_t dst, uint32_t *next_hop)
{
    int best_len = -1;
    uint32_t best_nh = 0;

    for (int i = 0; i < g_rt_len; ++i) {
        uint32_t net = g_rt[i].network;
        uint32_t mask = g_rt[i].mask;
        if (mask && (dst & mask) == net) {
            int l = prefix_len(mask);
            if (l > best_len) {
                best_len = l;
                best_nh = g_rt[i].gateway ? g_rt[i].gateway : dst;
            }
        }
    }
    if (best_len >= 0) {
        if (next_hop) *next_hop = best_nh;
        return true;
    }
    return false;
}
