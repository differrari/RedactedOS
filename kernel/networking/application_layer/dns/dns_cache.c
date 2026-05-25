#include "dns_cache.h"
#include "dns_wire.h"
#include "std/std.h"

typedef struct {
    uint8_t in_use;
    uint16_t rr_type;
    uint32_t name_len;
    char name[128];
    uint32_t ttl_ms;
    uint8_t addr[16];
} dns_cache_entry_t;

static dns_cache_entry_t g_dns_cache[32];
static bool g_dns_cache_inited = false;

static void dns_cache_ensure_init(void) {
    if (g_dns_cache_inited) return;
    g_dns_cache_inited= true;

    uint8_t a[16];
    memset(a, 0, sizeof(a));
    wr_be32(a, 0x7F000001u);
    dns_cache_put_ip("localhost", DNS_TYPE_A, a, 0xFFFFFFFFu);

    uint8_t v6[16];
    memset(v6, 0, sizeof(v6));
    v6[15] = 1;
    dns_cache_put_ip("localhost", DNS_TYPE_AAAA, v6, 0xFFFFFFFFu);
}

void dns_cache_put_ip(const char* name, uint16_t rr_type,const uint8_t addr[16], uint32_t ttl_ms) {
    if (!name || !addr) return;
    char norm[128];
    if (!dns_wire_name_normalize(name, norm, sizeof(norm))) return;
    uint32_t nlen = strlen(norm);
    if (!nlen) return;
    if (!ttl_ms) return;

    if (nlen == 9u&& memcmp(norm, "localhost", 9) == 0 && (rr_type == DNS_TYPE_A || rr_type == DNS_TYPE_AAAA)) ttl_ms = 0xFFFFFFFF;
    int free_i = -1;
    for (int i = 0; i < 32; i++) {
        if (!g_dns_cache[i].in_use) {
            if (free_i < 0) free_i = i;
            continue;
        }
        if (g_dns_cache[i].rr_type != rr_type) continue;
        if (g_dns_cache[i].name_len != nlen) continue;
        if (memcmp(g_dns_cache[i].name, norm, nlen) != 0) continue;
        memcpy(g_dns_cache[i].addr, addr, 16);
        g_dns_cache[i].ttl_ms = ttl_ms;
        return;
    }

    int idx = free_i;
    if (idx < 0) idx = 0;
    memset(&g_dns_cache[idx], 0, sizeof(g_dns_cache[idx]));
    g_dns_cache[idx].in_use = 1;
    g_dns_cache[idx].rr_type = rr_type;
    g_dns_cache[idx].name_len = nlen;
    memcpy(g_dns_cache[idx].name, norm, nlen);
    g_dns_cache[idx].name[nlen] = 0;
    g_dns_cache[idx].ttl_ms = ttl_ms;
    memcpy(g_dns_cache[idx].addr, addr, 16);
}

void dns_cache_remove_ip(const char* name, uint16_t rr_type) {
    char norm[128];
    if (!dns_wire_name_normalize(name, norm, sizeof(norm))) return;
    uint32_t nlen = strlen(norm);
    for (int i = 0; i < 32; i++) {
        if (!g_dns_cache[i].in_use) continue;
        if (g_dns_cache[i].rr_type != rr_type) continue;
        if (g_dns_cache[i].name_len != nlen) continue;
        if (memcmp(g_dns_cache[i].name, norm, nlen) != 0) continue;
        memset(&g_dns_cache[i], 0, sizeof(g_dns_cache[i]));
        return;
    }
}

bool dns_cache_get_ip(const char* name, uint16_t rr_type, uint8_t out_addr[16]) {
    dns_cache_ensure_init();
    if (!name || !out_addr) return false;
    char norm[128];
    if (!dns_wire_name_normalize(name, norm, sizeof(norm))) return false;
    uint32_t nlen = strlen(norm);
    if (!nlen) return false;
    for (int i = 0; i < 32; i++) {
        if (!g_dns_cache[i].in_use) continue;
        if (g_dns_cache[i].rr_type != rr_type) continue;
        if (g_dns_cache[i].ttl_ms == 0) continue;
        if (g_dns_cache[i].name_len != nlen) continue;
        if (memcmp(g_dns_cache[i].name, norm, nlen) != 0) continue;
        memcpy(out_addr, g_dns_cache[i].addr, 16);
        return true;
    }
    return false;
}

void dns_cache_tick(uint32_t ms) {
    dns_cache_ensure_init();
    for (int i = 0; i < 32; i++) {
        if (!g_dns_cache[i].in_use) continue;
        if (!g_dns_cache[i].ttl_ms) {
            g_dns_cache[i].in_use = 0;
            continue;
        }
        if (g_dns_cache[i].ttl_ms == 0xFFFFFFFFu) continue;
        if (g_dns_cache[i].ttl_ms <= ms) {
            memset(&g_dns_cache[i], 0, sizeof(g_dns_cache[i]));
        } else {
            g_dns_cache[i].ttl_ms -= ms;
        }
    }
}
