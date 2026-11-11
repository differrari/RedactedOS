#include "dns_cache.h"
#include "dns_wire.h"
#include "networking/interface_manager.h"
#include "std/memory.h"
#include "std/string.h"
#include "syscalls/syscalls.h"

typedef struct {
    uint8_t in_use;
    uint16_t rr_type;
    l3_id_t l3_id;
    uint32_t l3_epoch;
    uint32_t name_len;
    char name[128];
    uint64_t expire_ms;
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
    dns_cache_put_ip(0, "localhost", DNS_TYPE_A, a, 0xFFFFFFFFu);

    uint8_t v6[16];
    memset(v6, 0, sizeof(v6));
    v6[15] = 1;
    dns_cache_put_ip(0, "localhost", DNS_TYPE_AAAA, v6, 0xFFFFFFFFu);
}

void dns_cache_put_ip(l3_id_t l3_id, const char* name, uint16_t rr_type,const uint8_t addr[16], uint32_t ttl_ms) {
    if (!name || !addr) return;
    uint32_t epoch = l3_epoch(l3_id);
    if (l3_id && !epoch) return;
    dns_cache_ensure_init();
    char norm[128];
    if (!dns_wire_name_normalize(name, norm, sizeof(norm))) return;
    uint32_t nlen = strlen(norm);
    if (!nlen) return;
    if (!ttl_ms) return;
    uint32_t addr_len = rr_type == DNS_TYPE_A ? 4 : 16;

    if (nlen == 9u&& memcmp(norm, "localhost", 9) == 0 && (rr_type == DNS_TYPE_A || rr_type == DNS_TYPE_AAAA)) {
        for (int i = 0; i < 32; i++) {
            if (g_dns_cache[i].in_use && g_dns_cache[i].rr_type == rr_type && g_dns_cache[i].l3_id == l3_id && g_dns_cache[i].l3_epoch == epoch && g_dns_cache[i].name_len == nlen && !memcmp(g_dns_cache[i].name, norm, nlen)) return;
        }
        ttl_ms = UINT32_MAX;
    }
    int free_i = -1;
    int evict_i = -1;
    uint64_t evict_expire = UINT64_MAX;
    uint64_t now = get_time();
    for (int i = 0; i < 32; i++) {
        if (!g_dns_cache[i].in_use || (g_dns_cache[i].expire_ms != UINT64_MAX && now >= g_dns_cache[i].expire_ms)) {
            if (free_i < 0) free_i = i;
            continue;
        }
        if (g_dns_cache[i].expire_ms < evict_expire) {
            evict_expire = g_dns_cache[i].expire_ms;
            evict_i = i;
        }
        if (g_dns_cache[i].l3_id != l3_id || g_dns_cache[i].l3_epoch != epoch) continue;
        if (g_dns_cache[i].rr_type != rr_type) continue;
        if (g_dns_cache[i].name_len != nlen) continue;
        if (memcmp(g_dns_cache[i].name, norm, nlen) != 0) continue;
        memset(g_dns_cache[i].addr, 0, sizeof(g_dns_cache[i].addr));
        memcpy(g_dns_cache[i].addr, addr, addr_len);
        g_dns_cache[i].expire_ms = ttl_ms == 0xFFFFFFFFu ? UINT64_MAX : now + ttl_ms;
        return;
    }

    int idx = free_i;
    if (idx < 0) idx = evict_i;
    if (idx < 0) return;
    memset(&g_dns_cache[idx], 0, sizeof(g_dns_cache[idx]));
    g_dns_cache[idx].in_use = 1;
    g_dns_cache[idx].rr_type = rr_type;
    g_dns_cache[idx].l3_id = l3_id;
    g_dns_cache[idx].l3_epoch = epoch;
    g_dns_cache[idx].name_len = nlen;
    memcpy(g_dns_cache[idx].name, norm, nlen);
    g_dns_cache[idx].name[nlen] = 0;
    g_dns_cache[idx].expire_ms = ttl_ms == 0xFFFFFFFFu ? UINT64_MAX : now + ttl_ms;
    memcpy(g_dns_cache[idx].addr, addr, addr_len);
}

void dns_cache_remove_ip(l3_id_t l3_id, const char* name, uint16_t rr_type) {
    if (!name) return;
    uint32_t epoch = l3_epoch(l3_id);
    if (l3_id && !epoch) return;
    dns_cache_ensure_init();
    char norm[128];
    if (!dns_wire_name_normalize(name, norm, sizeof(norm))) return;
    uint32_t nlen = strlen(norm);
    if (nlen == 9 && memcmp(norm, "localhost", 9) == 0 && (rr_type == DNS_TYPE_A || rr_type == DNS_TYPE_AAAA)) return;
    for (int i = 0; i < 32; i++) {
        if (!g_dns_cache[i].in_use) continue;
        if (g_dns_cache[i].l3_id != l3_id || g_dns_cache[i].l3_epoch != epoch) continue;
        if (g_dns_cache[i].rr_type != rr_type) continue;
        if (g_dns_cache[i].name_len != nlen) continue;
        if (memcmp(g_dns_cache[i].name, norm, nlen) != 0) continue;
        memset(&g_dns_cache[i], 0, sizeof(g_dns_cache[i]));
        return;
    }
}

bool dns_cache_get_ip(l3_id_t l3_id, const char* name, uint16_t rr_type, uint8_t out_addr[16]) {
    if (!name || !out_addr) return false;
    uint32_t epoch = l3_epoch(l3_id);
    if (l3_id && !epoch) return false;
    dns_cache_ensure_init();
    char norm[128];
    if (!dns_wire_name_normalize(name, norm, sizeof(norm))) return false;
    uint32_t nlen = strlen(norm);
    if (!nlen) return false;
    if (nlen == 9 && !memcmp(norm, "localhost", 9)) {
        l3_id = 0;
        epoch = 0;
    }
    uint64_t now = get_time();
    for (int i = 0; i < 32; i++) {
        if (!g_dns_cache[i].in_use) continue;
        if (g_dns_cache[i].expire_ms != UINT64_MAX && now >= g_dns_cache[i].expire_ms) {
            memset(&g_dns_cache[i], 0, sizeof(g_dns_cache[i]));
            continue;
        }
        if (g_dns_cache[i].l3_id != l3_id || g_dns_cache[i].l3_epoch != epoch) continue;
        if (g_dns_cache[i].rr_type != rr_type) continue;
        if (g_dns_cache[i].name_len != nlen) continue;
        if (memcmp(g_dns_cache[i].name, norm, nlen) != 0) continue;
        memcpy(out_addr, g_dns_cache[i].addr, 16);
        return true;
    }
    return false;
}
