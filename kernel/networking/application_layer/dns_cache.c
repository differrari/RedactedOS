#include "dns_cache.h"
#include "std/std.h"

typedef struct {
    uint8_t in_use;
    uint8_t rr_type;
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
    dns_cache_put_ip("localhost", 1, a, 0xFFFFFFFFu);

    uint8_t v6[16];
    memset(v6, 0, sizeof(v6));
    v6[15] = 1;
    dns_cache_put_ip("localhost", 28, v6, 0xFFFFFFFFu);
}

void dns_cache_put_ip(const char* name, uint8_t rr_type,const uint8_t addr[16], uint32_t ttl_ms) {
    if (!name || !addr) return;
    uint32_t nlen = strlen(name);
    if (!nlen) return;
    if (nlen >= 128) return;
    if (!ttl_ms) return;

    if (nlen == 9u&& strncmp(name, "localhost", 9) == 0 && (rr_type == 1 || rr_type == 28))ttl_ms = 0xFFFFFFFFu;

    int free_i = -1;
    for (int i = 0; i < 32; i++) {
        if (!g_dns_cache[i].in_use) {
            if (free_i < 0) free_i = i;
            continue;
        }
        if (g_dns_cache[i].rr_type != rr_type) continue;
        if (g_dns_cache[i].name_len != nlen) continue;
        if (strncmp(g_dns_cache[i].name, name, (int)nlen) != 0) continue;
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
    memcpy(g_dns_cache[idx].name, name, nlen);
    g_dns_cache[idx].name[nlen] = 0;
    g_dns_cache[idx].ttl_ms = ttl_ms;
    memcpy(g_dns_cache[idx].addr, addr, 16);
}

bool dns_cache_get_ip(const char* name, uint8_t rr_type, uint8_t out_addr[16]) {
    dns_cache_ensure_init();
    if (!name || !out_addr) return false;
    uint32_t nlen = strlen(name);
    if (!nlen) return false;
    if (nlen >= 128) return false;
    for (int i = 0; i < 32; i++) {
        if (!g_dns_cache[i].in_use) continue;
        if (g_dns_cache[i].rr_type != rr_type) continue;
        if (g_dns_cache[i].ttl_ms == 0) continue;
        if (g_dns_cache[i].name_len != nlen) continue;
        if (strncmp(g_dns_cache[i].name, name, (int)nlen) != 0) continue;
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
