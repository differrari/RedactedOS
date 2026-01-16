#include "netpkt.h"
#include "std/std.h"
#include "syscalls/syscalls.h"

struct netpkt {
    uintptr_t base;
    uint32_t alloc;
    uint32_t head;
    uint32_t len;
    uint32_t refs;
    netpkt_free_fn free_fn;
    void* free_ctx;
};

static void netpkt_free_malloc(void* ctx, uintptr_t base, uint32_t alloc_size) {
    (void)ctx;
    if (base && alloc_size) free_sized((void*)base, alloc_size);
}

static bool netpkt_realloc_to(netpkt_t* p, uint32_t new_head, uint32_t new_alloc) {
    if (!p) return false;
    if (new_alloc < new_head + p->len) return false;

    uintptr_t nb = (uintptr_t)malloc(new_alloc);
    if (!nb) return false;

    if (p->len) memcpy((void*)(nb + new_head), (const void*)(p->base + p->head), p->len);
    if (p->free_fn) p->free_fn(p->free_ctx, p->base, p->alloc);

    p->base = nb;
    p->alloc = new_alloc;
    p->head = new_head;
    if (!p->free_fn) p->free_fn = netpkt_free_malloc;
    p->free_ctx = 0;
    return true;
}

netpkt_t* netpkt_alloc(uint32_t data_capacity, uint32_t headroom, uint32_t tailroom) {
    uint32_t alloc = headroom+ data_capacity + tailroom;
    if (alloc == 0) alloc = 1;

    uintptr_t base = (uintptr_t)malloc(alloc);
    if (!base) return 0;

    netpkt_t* p = (netpkt_t*)malloc(sizeof(netpkt_t));
    if (!p) {
        free_sized((void*)base, alloc);
        return 0;
    }

    p->base = base;
    p->alloc = alloc;
    p->head = headroom;
    p->len = 0;
    p->refs = 1;
    p->free_fn = netpkt_free_malloc;
    p->free_ctx = 0;
    return p;
}

netpkt_t* netpkt_wrap(uintptr_t base, uint32_t alloc_size, uint32_t data_len, netpkt_free_fn free_fn, void* ctx) {
    if (!base || !alloc_size) return 0;
    if (data_len > alloc_size) return 0;

    netpkt_t* p = (netpkt_t*)malloc(sizeof(netpkt_t));
    if (!p) return 0;

    p->base = base;
    p->alloc = alloc_size;
    p->head = 0;
    p->len = data_len;
    p->refs = 1;
    p->free_fn = free_fn ? free_fn : netpkt_free_malloc;
    p->free_ctx = ctx;
    return p;
}

void netpkt_ref(netpkt_t* p){
    if (p)p->refs++;
}

void netpkt_unref(netpkt_t* p) {
    if (!p) return;
    if (p->refs > 1) {
        p->refs--;
        return;
    }
    if (p->free_fn) p->free_fn(p->free_ctx, p->base, p->alloc);
    free_sized(p, sizeof(*p));
}

uintptr_t netpkt_data(const netpkt_t* p) {
    if (!p) return 0;
    return p->base + p->head;
}

uint32_t netpkt_len(const netpkt_t* p) {
    return p ? p->len : 0;
}

uint32_t netpkt_headroom(const netpkt_t* p) {
    return p ? p->head : 0;
}

uint32_t netpkt_tailroom(const netpkt_t* p) {
    if (!p) return 0;
    uint32_t used = p->head + p->len;
    return used >= p->alloc ? 0: (p->alloc - used);
}

bool netpkt_ensure_headroom(netpkt_t* p, uint32_t need) {
    if (!p) return false;
    if (p->head >= need) return true;

    uint32_t tail = netpkt_tailroom(p);
    uint32_t new_head = need;
    uint32_t new_alloc = new_head + p->len + tail;
    if (new_alloc < p->alloc + (need - p->head)) new_alloc = p->alloc + (need - p->head);
    return netpkt_realloc_to(p, new_head, new_alloc);
}

bool netpkt_ensure_tailroom(netpkt_t* p, uint32_t need) {
    if (!p) return false;
    if (netpkt_tailroom(p) >= need) return true;

    uint32_t new_alloc = p->head + p->len + need;
    if (new_alloc < p->alloc + (need - netpkt_tailroom(p))) new_alloc = p->alloc + (need - netpkt_tailroom(p));
    return netpkt_realloc_to(p, p->head, new_alloc);
}

void* netpkt_push(netpkt_t* p, uint32_t bytes) {
    if (!p || bytes == 0) return (void*)netpkt_data(p);
    if (!netpkt_ensure_headroom(p, bytes)) return 0;
    p->head -= bytes;
    p->len += bytes;
    return (void*)(p->base + p->head);
}

void* netpkt_put(netpkt_t* p, uint32_t bytes) {
    if (!p || bytes == 0) return (void*)(netpkt_data(p) + p->len);
    if (!netpkt_ensure_tailroom(p, bytes)) return 0;
    uintptr_t out = p->base + p->head + p->len;
    p->len += bytes;
    return (void*)out;
}

bool netpkt_pull(netpkt_t* p, uint32_t bytes) {
    if (!p) return false;
    if (bytes > p->len) return false;
    p->head += bytes;
    p->len -= bytes;
    return true;
}

bool netpkt_trim(netpkt_t* p, uint32_t new_len) {
    if (!p) return false;
    if (new_len > p->len) return false;
    p->len = new_len;
    return true;
}
