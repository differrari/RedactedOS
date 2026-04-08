#include "netpkt.h"
#include "std/std.h"
#include "memory/page_allocator.h"

#define NETPKT_F_VIEW 1u
#define NETPKT_BUF_F_EXTERNAL 1u
#define NETPKT_META_KEEP_EMPTY_PAGES 2u

typedef struct netpkt_buf netpkt_buf_t;

typedef struct meta_slab_page meta_slab_page_t;

typedef struct meta_obj_hdr {
    meta_slab_page_t*page;
    uintptr_t pad;
} meta_obj_hdr_t;

struct meta_slab_page {
    meta_slab_page_t* next;
    void* free_list;
    uint16_t in_use;
    uint16_t capacity;
    uint32_t stride;
};

typedef struct meta_slab {
    meta_slab_page_t* partial;
    meta_slab_page_t* full;
    meta_slab_page_t* empty;
    uint32_t obj_size;
    uint32_t stride;
    uint32_t empty_pages;
    uint32_t keep_empty;
} meta_slab_t;

struct netpkt_buf {
    uintptr_t base;
    uint32_t alloc;
    uint32_t refs;
    uint32_t flags;
    netpkt_free_fn free_fn;
    void* free_ctx;
};

struct netpkt {
    netpkt_buf_t* buf;
    uint32_t off;
    uint32_t cap;
    uint32_t head;
    uint32_t len;
    uint32_t refs;
    uint32_t flags;
};

static uint64_t g_netpkt_page_bytes;
static uint64_t g_netpkt_payload_page_bytes;
static uint64_t g_netpkt_meta_page_bytes;

static meta_slab_t g_meta_slab_pkt;
static meta_slab_t g_meta_slab_buf;

static uintptr_t g_spare_page;

static void* meta_slab_alloc(meta_slab_t* s, uint32_t obj_size_aligned) {
    if (!s) return 0;
    if (!obj_size_aligned) return 0;
    if ((obj_size_aligned & 15u) != 0u) return 0;

    if (!s->obj_size) {
        s->obj_size = obj_size_aligned;
        s->stride = (uint32_t)(((uint64_t)sizeof(meta_obj_hdr_t) + (uint64_t)obj_size_aligned + 15ull) & ~15ull);
        if (!s->stride) return 0;
        s->keep_empty = NETPKT_META_KEEP_EMPTY_PAGES;
    } else {
        if (s->obj_size != obj_size_aligned) return 0;
        if (!s->stride) return 0;
    }

    if (!s->partial) {
        if (s->empty) {
            meta_slab_page_t* p =s->empty;
            s->empty = p->next;
            if (s->empty_pages) s->empty_pages--;
            p->next = s->partial;
        s->partial = p;
        } else {
            if ((uint64_t)PAGE_SIZE > (uint64_t)NETPKT_MAX_PAGE_BYTES) return 0;
            if (g_netpkt_page_bytes > (uint64_t)NETPKT_MAX_PAGE_BYTES - (uint64_t)PAGE_SIZE) return 0;

            g_netpkt_page_bytes += (uint64_t)PAGE_SIZE;
            g_netpkt_meta_page_bytes += (uint64_t)PAGE_SIZE;

            meta_slab_page_t* p = (meta_slab_page_t*)palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW | MEM_NORM, true);
            if (!p) {
                if (g_netpkt_page_bytes >= (uint64_t)PAGE_SIZE) g_netpkt_page_bytes-= (uint64_t)PAGE_SIZE;
                else g_netpkt_page_bytes = 0;

                if (g_netpkt_meta_page_bytes >= (uint64_t)PAGE_SIZE) g_netpkt_meta_page_bytes -= (uint64_t)PAGE_SIZE;
                else g_netpkt_meta_page_bytes = 0;
                return 0;
            }

            p->next = 0;//
            p->free_list = 0;
            p->in_use = 0;
            p->capacity = 0;
            p->stride = s->stride;

            uint8_t* start = (uint8_t*)p + (uint64_t)sizeof(*p);
            start = (uint8_t*)(((uintptr_t)start + 15u) &~(uintptr_t)15u);

            uint64_t avail = (uint64_t)PAGE_SIZE - (uint64_t)(start - (uint8_t*)p);
            if (avail < (uint64_t)p->stride) {
                pfree(p, (uint64_t)PAGE_SIZE);
                if (g_netpkt_page_bytes >= (uint64_t)PAGE_SIZE) g_netpkt_page_bytes -= (uint64_t)PAGE_SIZE;
                else g_netpkt_page_bytes = 0;

                if (g_netpkt_meta_page_bytes >= (uint64_t)PAGE_SIZE) g_netpkt_meta_page_bytes -= (uint64_t)PAGE_SIZE;
                else g_netpkt_meta_page_bytes = 0;
                return 0;
            }

            uint32_t cap = (uint32_t)(avail / (uint64_t)p->stride);
            if (!cap) {
                pfree(p, (uint64_t)PAGE_SIZE);
                if (g_netpkt_page_bytes >= (uint64_t)PAGE_SIZE) g_netpkt_page_bytes -= (uint64_t)PAGE_SIZE;
                else g_netpkt_page_bytes = 0;
                if (g_netpkt_meta_page_bytes >= (uint64_t)PAGE_SIZE) g_netpkt_meta_page_bytes -= (uint64_t)PAGE_SIZE;
                else g_netpkt_meta_page_bytes = 0;
                return 0;
            }

            p->capacity = (uint16_t)cap;

            uint32_t i = 0;
            while (i< cap) {
                uint8_t* slot = start + (uint64_t)i * (uint64_t)p->stride;
                meta_obj_hdr_t* h = (meta_obj_hdr_t*)(uintptr_t)slot;
                h->page = p;
                h->pad = 0;
                void* obj = (void*)(uintptr_t)(slot + (uint64_t)sizeof(*h));

                *(void**)obj = p->free_list;
                p->free_list = obj;
                i++;
            }

            p->next = s->partial;
            s->partial = p;
        }
    }

    meta_slab_page_t* page = s->partial;
    if (!page) return 0;
    if (!page->free_list) return 0;

    void* obj = page->free_list;
    page->free_list = *(void**)obj;
    page->in_use++;

    if (page->in_use == page->capacity) {
        s->partial = page->next;
        page->next = s->full;
        s->full = page;
    }

    return obj;
}

static void meta_slab_free(meta_slab_t* s, void* obj) {
    if (!s) return;
    if (!obj) return;

    meta_obj_hdr_t* h = (meta_obj_hdr_t*)(uintptr_t)obj -1;
    meta_slab_page_t* page = h->page;
    if (!page) return;

    *(void**)obj = page->free_list;
    page->free_list = obj;

    bool was_full = false;
    if (page->in_use == page->capacity) was_full = true;
    if (page->in_use) page->in_use--;

    if (was_full) {
        meta_slab_page_t* prev = 0;
        meta_slab_page_t* cur = s->full;
        while (cur && cur != page) {
            prev = cur;
            cur = cur->next;
        }
        if (cur) {
            if (prev) prev->next = cur->next;
            else s->full = cur->next;
            cur->next = s->partial;
            s->partial = cur;
        }
    }

    if (!page->in_use) {
        meta_slab_page_t* prev = 0;
        meta_slab_page_t* cur = s->partial;
        while (cur && cur != page) {
            prev = cur;
            cur = cur->next;
        }
        if (cur) {
            if (prev) prev->next = cur->next;
            else s->partial = cur->next;
        } else {
            prev = 0;
            cur = s->full;
            while (cur && cur != page) {
                prev = cur;
                cur = cur->next;
            }
            if (cur) {
                if (prev) prev->next = cur->next;
                else s->full = cur->next;
            }
        }

        page->next = s->empty;
        s->empty = page;
        s->empty_pages++;

        while (s->empty_pages > s->keep_empty){
            meta_slab_page_t* e = s->empty;
            if (!e) break;
            s->empty = e->next;
            s->empty_pages--;
            pfree(e,(uint64_t)PAGE_SIZE);
            if (g_netpkt_page_bytes >= (uint64_t)PAGE_SIZE) g_netpkt_page_bytes -= (uint64_t)PAGE_SIZE;
            else g_netpkt_page_bytes = 0;
            if (g_netpkt_meta_page_bytes >= (uint64_t)PAGE_SIZE) g_netpkt_meta_page_bytes -= (uint64_t)PAGE_SIZE;
            else g_netpkt_meta_page_bytes = 0;
        }
    }
}

static bool netpkt_realloc_to(netpkt_t* p, uint32_t new_head, uint32_t new_alloc) {
    if (!p) return false;
    if (!p->buf) return false;
    if (p->flags & NETPKT_F_VIEW) return false;

    uint64_t min = (uint64_t)new_head + (uint64_t)p->len;
    if ((uint64_t)new_alloc < min) return false;

    uint64_t bytes = (uint64_t)new_alloc;
    if (!bytes) bytes = 1;
    uint64_t cap64 = count_pages(bytes, PAGE_SIZE)*(uint64_t)PAGE_SIZE;
    if (!cap64) cap64 = PAGE_SIZE;
    if (cap64 > (uint64_t)NETPKT_MAX_ALLOC) return false;

    uint32_t cap = (uint32_t)cap64;
    if ((uint64_t)new_head+(uint64_t)p->len > (uint64_t)cap) return false;

    void* mem = 0;
    bool from_spare = false;
    if (cap == PAGE_SIZE && g_spare_page) {
        mem = (void*)g_spare_page;
        g_spare_page = 0;
        from_spare = true;
        memset(mem, 0, PAGE_SIZE);
    } else {
        if ((uint64_t)cap > (uint64_t)NETPKT_MAX_PAGE_BYTES) return false;
        if (g_netpkt_page_bytes > (uint64_t)NETPKT_MAX_PAGE_BYTES - (uint64_t)cap)return false;
        g_netpkt_page_bytes += (uint64_t)cap;
        g_netpkt_payload_page_bytes += (uint64_t)cap;

        mem = palloc((uint64_t)cap, MEM_PRIV_KERNEL, MEM_RW | MEM_NORM, true);
        if (!mem) {
            if (g_netpkt_page_bytes >= (uint64_t)cap) g_netpkt_page_bytes -= (uint64_t)cap;
            else g_netpkt_page_bytes = 0;

            if (g_netpkt_payload_page_bytes >= (uint64_t)cap) g_netpkt_payload_page_bytes -= (uint64_t)cap;
            else g_netpkt_payload_page_bytes = 0;
            return false;
        }
    }

    uint32_t sz = (uint32_t)(((uint64_t)sizeof(netpkt_buf_t) + 15) & ~15ull);
    netpkt_buf_t* nb = (netpkt_buf_t*)meta_slab_alloc(&g_meta_slab_buf, sz);
    if (!nb) {
        if (from_spare) {
            g_spare_page = (uintptr_t)mem;
        } else {
            pfree(mem, (uint64_t)cap);
            if (g_netpkt_page_bytes >= (uint64_t)cap) g_netpkt_page_bytes -= (uint64_t)cap;
            else g_netpkt_page_bytes = 0;
            if (g_netpkt_payload_page_bytes >= (uint64_t)cap) g_netpkt_payload_page_bytes -= (uint64_t)cap;
            else g_netpkt_payload_page_bytes = 0;
        }
        return false;
    }

    nb->base = (uintptr_t)mem;
    nb->alloc = cap;
    nb->refs = 1;
    nb->flags = 0;
    nb->free_fn = 0;
    nb->free_ctx = 0;

    if (p->len) memcpy((void*)(nb->base + (uintptr_t)new_head), (const void*)netpkt_data(p), p->len);

    netpkt_buf_t* ob = p->buf;
    p->buf = nb;
    p->off = 0;
    p->cap = cap;
    p->head = new_head;

    if (ob) {
        if (ob->refs > 1) {
            ob->refs--;
        } else {
            if (ob->flags & NETPKT_BUF_F_EXTERNAL) {
                if (ob->free_fn) ob->free_fn(ob->free_ctx, ob->base, ob->alloc);
            } else {
                bool aligned = ((ob->base & (PAGE_SIZE - 1)) == 0) && ((ob->alloc & (PAGE_SIZE - 1)) == 0);
                if (!g_spare_page && aligned) {
                    g_spare_page = ob->base;

                    if (ob->alloc > PAGE_SIZE) {
                        pfree((void*)(ob->base + PAGE_SIZE), (uint64_t)ob->alloc - (uint64_t)PAGE_SIZE);
                        uint64_t dec = (uint64_t)ob->alloc-(uint64_t)PAGE_SIZE;
                        if (g_netpkt_page_bytes >= dec) g_netpkt_page_bytes -= dec;
                        else g_netpkt_page_bytes = 0;
                        if (g_netpkt_payload_page_bytes >= dec) g_netpkt_payload_page_bytes -= dec;
                        else g_netpkt_payload_page_bytes = 0;
                    }
                } else {
                    if (ob->base) pfree((void*)ob->base,(uint64_t)ob->alloc);
                    uint64_t dec = (uint64_t)ob->alloc;
                    if (g_netpkt_page_bytes >= dec) g_netpkt_page_bytes -= dec;
                    else g_netpkt_page_bytes = 0;
                    if (g_netpkt_payload_page_bytes >= dec) g_netpkt_payload_page_bytes -= dec;
                    else g_netpkt_payload_page_bytes = 0;
                }
            }

            meta_slab_free(&g_meta_slab_buf, ob);
        }
    }

    return true;
}

netpkt_t* netpkt_alloc(uint32_t data_capacity, uint32_t headroom, uint32_t tailroom) {
    uint64_t alloc = (uint64_t)headroom + (uint64_t)data_capacity + (uint64_t)tailroom;
    if (alloc > (uint64_t)NETPKT_MAX_ALLOC) return 0;

    if (!alloc) alloc = 1;
    uint64_t cap64 = count_pages(alloc, PAGE_SIZE) * (uint64_t)PAGE_SIZE;
    if (!cap64) cap64 = PAGE_SIZE;
    if (cap64 > (uint64_t)NETPKT_MAX_ALLOC) return 0;

    uint32_t cap = (uint32_t)cap64;

    void* mem = 0;
    bool from_spare = false;
    if (cap == PAGE_SIZE && g_spare_page) {
        mem = (void*)g_spare_page;
        g_spare_page = 0;
        from_spare = true;
        memset(mem, 0, PAGE_SIZE);
    } else {
        if ((uint64_t)cap > (uint64_t)NETPKT_MAX_PAGE_BYTES) return 0;
        if (g_netpkt_page_bytes > (uint64_t)NETPKT_MAX_PAGE_BYTES - (uint64_t)cap) return 0;
        g_netpkt_page_bytes += (uint64_t)cap;
        g_netpkt_payload_page_bytes += (uint64_t)cap;

        mem = palloc((uint64_t)cap, MEM_PRIV_KERNEL, MEM_RW | MEM_NORM, true);
        if (!mem) {
            if (g_netpkt_page_bytes >= (uint64_t)cap) g_netpkt_page_bytes -= (uint64_t)cap;
            else g_netpkt_page_bytes = 0;
            if (g_netpkt_payload_page_bytes >= (uint64_t)cap) g_netpkt_payload_page_bytes -= (uint64_t)cap;
            else g_netpkt_payload_page_bytes = 0;
            return 0;
        }
    }

    uint32_t bsz = (uint32_t)(((uint64_t)sizeof(netpkt_buf_t) + 15ull) &~15ull);
    netpkt_buf_t* b = (netpkt_buf_t*)meta_slab_alloc(&g_meta_slab_buf, bsz);
    if (!b) {
        if (from_spare) {
            g_spare_page = (uintptr_t)mem;
        } else {
            pfree(mem, (uint64_t)cap);
            if (g_netpkt_page_bytes >= (uint64_t)cap) g_netpkt_page_bytes -= (uint64_t)cap;
            else g_netpkt_page_bytes = 0;
            if (g_netpkt_payload_page_bytes >= (uint64_t)cap) g_netpkt_payload_page_bytes -= (uint64_t)cap;
            else g_netpkt_payload_page_bytes = 0;
        }
        return 0;
    }

    b->base = (uintptr_t)mem;
    b->alloc = cap;
    b->refs = 1;
    b->flags = 0;
    b->free_fn = 0;
    b->free_ctx = 0;

    uint32_t psz = (uint32_t)(((uint64_t)sizeof(netpkt_t) + 15ull) &~15ull);
    netpkt_t* p = (netpkt_t*)meta_slab_alloc(&g_meta_slab_pkt, psz);
    if (!p) {
        meta_slab_free(&g_meta_slab_buf, b);
        if (from_spare) {
            g_spare_page = (uintptr_t)mem;
        } else {
            pfree(mem, (uint64_t)cap);
            if (g_netpkt_page_bytes >= (uint64_t)cap) g_netpkt_page_bytes -= (uint64_t)cap;
            else g_netpkt_page_bytes = 0;
            if (g_netpkt_payload_page_bytes >= (uint64_t)cap) g_netpkt_payload_page_bytes -= (uint64_t)cap;
            else g_netpkt_payload_page_bytes = 0;
        }
        return 0;
    }

    p->buf = b;
    p->off = 0;
    p->cap = cap;
    p->head = headroom;
    p->len = 0;
    p->refs = 1;
    p->flags = 0;
    return p;
}

netpkt_t* netpkt_wrap(uintptr_t base, uint32_t alloc_size, uint32_t data_off, uint32_t data_len, netpkt_free_fn free_fn, void* ctx) {
    if (!base) return 0;
    if (!alloc_size) return 0;
    if (alloc_size > NETPKT_MAX_ALLOC) return 0;
    if (data_off > alloc_size) return 0;
    if (data_len > alloc_size - data_off) return 0;

    uint32_t bsz = (uint32_t)(((uint64_t)sizeof(netpkt_buf_t) + 15ull) &~15ull);
    netpkt_buf_t* b = (netpkt_buf_t*)meta_slab_alloc(&g_meta_slab_buf, bsz);
    if (!b) return 0;

    b->base = base;
    b->alloc = alloc_size;
    b->refs = 1;
    b->flags = NETPKT_BUF_F_EXTERNAL;
    b->free_fn = free_fn;
    b->free_ctx = ctx;

    uint32_t psz = (uint32_t)(((uint64_t)sizeof(netpkt_t) + 15ull) &~15ull);
    netpkt_t* p = (netpkt_t*)meta_slab_alloc(&g_meta_slab_pkt, psz);
    if (!p) {
        meta_slab_free(&g_meta_slab_buf, b);
        return 0;
    }

    p->buf = b;
    p->off = 0;
    p->cap = alloc_size;
    p->head = data_off;
    p->len = data_len;
    p->refs = 1;
    p->flags = 0;
    return p;
}

netpkt_t* netpkt_view(netpkt_t* parent, uint32_t off, uint32_t len) {
    if (!parent) return 0;
    if (!parent->buf) return 0;

    uint64_t end = (uint64_t)off+(uint64_t)len;
    if (end > (uint64_t)parent->len) return 0;

    uint64_t abs = (uint64_t)parent->off + (uint64_t)parent->head + (uint64_t)off;
    if (abs + (uint64_t)len > (uint64_t)parent->buf->alloc) return 0;

    uint32_t psz = (uint32_t)(((uint64_t)sizeof(netpkt_t) + 15ull) &~15ull);
    netpkt_t* v = (netpkt_t*)meta_slab_alloc(&g_meta_slab_pkt, psz);
    if (!v) return 0;

    parent->buf->refs++;

    v->buf = parent->buf;
    v->off = (uint32_t)abs;
    v->cap = len;
    v->head = 0;
    v->len = len;
    v->refs = 1;
    v->flags = NETPKT_F_VIEW;
    return v;
}

void netpkt_ref(netpkt_t* p){
    if (!p) return;
    p->refs++;
}

void netpkt_unref(netpkt_t* p) {
    if (!p) return;
    if (p->refs > 1) {
        p->refs--;
        return;
    }

    netpkt_buf_t* b = p->buf;
    if (b) {
        if (b->refs > 1) {
            b->refs--;
        } else {
            if (b->flags & NETPKT_BUF_F_EXTERNAL) {
                if (b->free_fn) b->free_fn(b->free_ctx, b->base, b->alloc);
            } else {
                bool aligned = ((b->base & (PAGE_SIZE - 1)) == 0) && ((b->alloc & (PAGE_SIZE - 1)) == 0);
                if (!g_spare_page && aligned) {
                    g_spare_page = b->base;
                    if (b->alloc > PAGE_SIZE) {
                        pfree((void*)(b->base + PAGE_SIZE), (uint64_t)b->alloc-(uint64_t)PAGE_SIZE);
                        uint64_t dec = (uint64_t)b->alloc-(uint64_t)PAGE_SIZE;
                        if (g_netpkt_page_bytes >= dec) g_netpkt_page_bytes -= dec;
                        else g_netpkt_page_bytes = 0;
                        if (g_netpkt_payload_page_bytes >= dec) g_netpkt_payload_page_bytes -= dec;
                        else g_netpkt_payload_page_bytes = 0;
                    }
                } else {
                    if (b->base) pfree((void*)b->base, (uint64_t)b->alloc);
                    uint64_t dec = (uint64_t)b->alloc;
                    if (g_netpkt_page_bytes >= dec) g_netpkt_page_bytes -= dec;
                    else g_netpkt_page_bytes = 0;
                    if (g_netpkt_payload_page_bytes >= dec) g_netpkt_payload_page_bytes -= dec;
                    else g_netpkt_payload_page_bytes = 0;
                }
            }

            meta_slab_free(&g_meta_slab_buf, b);
        }
    }

    meta_slab_free(&g_meta_slab_pkt, p);
}

uintptr_t netpkt_data(const netpkt_t* p) {
    if (!p) return 0;
    if (!p->buf) return 0;
    return p->buf->base+(uintptr_t)p->off+(uintptr_t)p->head;
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
    return used >= p->cap ? 0: (p->cap - used);
}

bool netpkt_ensure_headroom(netpkt_t* p, uint32_t need) {
    if (!p) return false;
    if (!p->buf) return false;
    if (p->flags & NETPKT_F_VIEW) return false;
    if (need > NETPKT_MAX_ALLOC) return false;

    if (p->head >= need) {
        if (p->buf->refs == 1) return true;
        return netpkt_realloc_to(p, p->head, p->cap);
    }

    uint32_t tail = netpkt_tailroom(p);
    uint32_t new_head = need;

    uint64_t alloc = (uint64_t)new_head + (uint64_t)p->len + (uint64_t)tail;
    uint64_t min = (uint64_t)p->cap + (uint64_t)(need - p->head);
    if (alloc < min) alloc = min;
    if (alloc > (uint64_t)NETPKT_MAX_ALLOC) return false;

    return netpkt_realloc_to(p, new_head, (uint32_t)alloc);
}

bool netpkt_ensure_tailroom(netpkt_t* p, uint32_t need) {
    if (!p) return false;
    if (!p->buf) return false;
    if (p->flags & NETPKT_F_VIEW) return false;
    if (need > NETPKT_MAX_ALLOC) return false;

    uint32_t tail = netpkt_tailroom(p);
    if (tail >= need) {
        if (p->buf->refs == 1) return true;
        return netpkt_realloc_to(p, p->head, p->cap);
    }

    uint64_t alloc = (uint64_t)p->head + (uint64_t)p->len + (uint64_t)need;
    uint64_t min = (uint64_t)p->cap + (uint64_t)(need - tail);
    if (alloc < min) alloc = min;
    if (alloc > (uint64_t)NETPKT_MAX_ALLOC) return false;

    return netpkt_realloc_to(p, p->head, (uint32_t)alloc);
}

void* netpkt_push(netpkt_t* p, uint32_t bytes) {
    if (!p) return 0;

    if ((p->flags & NETPKT_F_VIEW) && bytes) return 0;
    if (!bytes) return (void*)netpkt_data(p);
    if (!netpkt_ensure_headroom(p, bytes)) return 0;

    p->head -= bytes;
    p->len += bytes;
    return (void*)(p->buf->base + (uintptr_t)p->off + (uintptr_t)p->head);
}

void* netpkt_put(netpkt_t* p, uint32_t bytes) {
    if (!p) return 0;
    if ((p->flags & NETPKT_F_VIEW) && bytes) return 0;
    if (!bytes) return (void*)(netpkt_data(p) + (uintptr_t)p->len);
    if (!netpkt_ensure_tailroom(p, bytes)) return 0;

    uintptr_t out = p->buf->base+(uintptr_t)p->off + (uintptr_t)p->head + (uintptr_t)p->len;
    p->len += bytes;
    return (void*)out;
}

bool netpkt_pull(netpkt_t* p, uint32_t bytes) {
    if (!p) return false;
    if (bytes > p->len) return false;
    p->head += bytes;
    p->len -= bytes;

    if (p->flags & NETPKT_F_VIEW) return true;
    if (!p->buf) return true;
    if (p->buf->flags & NETPKT_BUF_F_EXTERNAL) return true;
    if (p->buf->refs != 1) return true;
    if (p->cap <= PAGE_SIZE) return true;

    uint64_t need = (uint64_t)p->head+(uint64_t)p->len;
    if (!need) need = 1;
    uint64_t newcap64 = count_pages(need, PAGE_SIZE) * (uint64_t)PAGE_SIZE;
    if (newcap64 < PAGE_SIZE) newcap64 = PAGE_SIZE;
    if (newcap64 >= (uint64_t)p->cap) return true;

    uint32_t newcap = (uint32_t)newcap64;
    uint64_t dec = (uint64_t)p->buf->alloc - (uint64_t)newcap;
    if (dec && ((p->buf->base & (PAGE_SIZE - 1)) == 0)) {
        pfree((void*)(p->buf->base + (uintptr_t)newcap), dec);
        if (g_netpkt_page_bytes >= dec) g_netpkt_page_bytes -= dec;
        else g_netpkt_page_bytes = 0;
        if (g_netpkt_payload_page_bytes >= dec) g_netpkt_payload_page_bytes -= dec;
        else g_netpkt_payload_page_bytes = 0;
        p->buf->alloc = newcap;
        p->cap = newcap;
    }

    return true;
}

bool netpkt_trim(netpkt_t* p, uint32_t new_len) {
    if (!p) return false;
    if (new_len > p->len) return false;
    p->len = new_len;

    if (p->flags & NETPKT_F_VIEW) return true;
    if (!p->buf) return true;
    if (p->buf->flags & NETPKT_BUF_F_EXTERNAL) return true;
    if (p->buf->refs != 1) return true;
    if (p->cap <= PAGE_SIZE) return true;

    uint64_t need = (uint64_t)p->head + (uint64_t)p->len;
    if (!need) need = 1;
    uint64_t newcap64 = count_pages(need, PAGE_SIZE) * (uint64_t)PAGE_SIZE;
    if (newcap64 < PAGE_SIZE) newcap64 = PAGE_SIZE;
    if (newcap64 >= (uint64_t)p->cap) return true;

    uint32_t newcap = (uint32_t)newcap64;
    uint64_t dec = (uint64_t)p->buf->alloc - (uint64_t)newcap;
    if (dec && ((p->buf->base & (PAGE_SIZE - 1)) == 0)) {
        pfree((void*)(p->buf->base + (uintptr_t)newcap), dec);
        if (g_netpkt_page_bytes >= dec) g_netpkt_page_bytes -= dec;
        else g_netpkt_page_bytes = 0;
        if (g_netpkt_payload_page_bytes >= dec) g_netpkt_payload_page_bytes -= dec;
        else g_netpkt_payload_page_bytes = 0;
        p->buf->alloc = newcap;
        p->cap = newcap;
    }

    return true;
}
