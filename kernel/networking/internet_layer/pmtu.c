#include "pmtu.h"
#include "std/memory.h"
#include "syscalls/syscalls.h"
#include "exceptions/irq.h"

#define PMTU_CACHE_MAX_AGE_MS 600000u
#define PMTU_CACHE_MAX_ENTRIES 128u

typedef struct {
    uint8_t dst[16];
    uint32_t l3_epoch;
    uint32_t timestamp_ms;
    l3_id_t l3_id;
    uint16_t mtu;
    ip_version_t ver;
    bool used;
} pmtu_entry_t;

static pmtu_entry_t g_pmtu[PMTU_CACHE_MAX_ENTRIES];

uint16_t pmtu_get(l3_id_t l3_id, uint32_t l3_epoch, ip_version_t ver, const void* dst) {
    if (!l3_id || !l3_epoch || !dst || (ver != IP_VER4 && ver != IP_VER6)) return 0;

    uint32_t dst_len = ver == IP_VER6 ? 16 : 4;
    uint32_t now = (uint32_t)get_time();
    irq_flags_t irq = irq_save_disable();

    for (uint32_t i = 0; i < PMTU_CACHE_MAX_ENTRIES; i++) {
        pmtu_entry_t* e = &g_pmtu[i];
        if (!e->used) continue;
        if (now - e->timestamp_ms >= PMTU_CACHE_MAX_AGE_MS || (e->l3_id == l3_id && e->l3_epoch != l3_epoch)) {
            memset(e, 0, sizeof(*e));
            continue;
        }
        if (e->l3_id != l3_id || e->ver != ver || memcmp(e->dst, dst, dst_len) != 0) continue;

        uint16_t mtu = e->mtu;
        irq_restore(irq);
        return mtu;
    }

    irq_restore(irq);
    return 0;
}

uint16_t pmtu_note(l3_id_t l3_id, uint32_t l3_epoch, ip_version_t ver, const void* dst, uint16_t mtu) {
    if (!l3_id || !l3_epoch || !dst || !mtu || (ver != IP_VER4 && ver != IP_VER6)) return 0;

    uint32_t dst_len = ver == IP_VER6 ? 16 : 4;
    uint32_t now = (uint32_t)get_time();
    int free_slot = -1;
    int oldest = -1;
    uint32_t oldest_age = 0;
    irq_flags_t irq = irq_save_disable();

    for (uint32_t i = 0; i < PMTU_CACHE_MAX_ENTRIES; i++) {
        pmtu_entry_t* e = &g_pmtu[i];
        if (!e->used) {
            if (free_slot < 0) free_slot = (int)i;
            continue;
        }
        if (now - e->timestamp_ms >= PMTU_CACHE_MAX_AGE_MS || (e->l3_id == l3_id && e->l3_epoch != l3_epoch)) {
            memset(e, 0, sizeof(*e));
            if (free_slot < 0) free_slot = (int)i;
            continue;
        }
        if (e->l3_id == l3_id && e->ver == ver && memcmp(e->dst, dst, dst_len) == 0) {
            if (mtu < e->mtu) e->mtu = mtu;
            e->timestamp_ms = now;
            mtu = e->mtu;
            irq_restore(irq);
            return mtu;
        }

        uint32_t age = now - e->timestamp_ms;
        if (oldest < 0 || age > oldest_age) {
            oldest = (int)i;
            oldest_age = age;
        }
    }

    int slot = free_slot >= 0 ? free_slot : oldest;
    if (slot < 0) {
        irq_restore(irq);
        return 0;
    }

    pmtu_entry_t* e = &g_pmtu[slot];
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->ver = ver;
    e->l3_id = l3_id;
    e->l3_epoch = l3_epoch;
    e->mtu = mtu;
    e->timestamp_ms = now;
    memcpy(e->dst, dst, dst_len);
    irq_restore(irq);
    return mtu;
}
