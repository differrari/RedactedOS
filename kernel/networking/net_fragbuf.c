#include "networking/net_fragbuf.h"
#include "std/memory.h"
#include "std/string.h"

void net_fragbuf_init(net_fragbuf_t *fb) {
    if (!fb) return;
    *fb = (net_fragbuf_t){0};
    fb->max_len = NET_FRAGBUF_DEFAULT_MAX_LEN;
    fb->step = NET_FRAGBUF_DEFAULT_STEP;
}

void net_fragbuf_free(net_fragbuf_t *fb) {
    if (!fb) return;
    if (fb->data) release(fb->data);
    if (fb->blocks) release(fb->blocks);
    *fb = (net_fragbuf_t){0};
}

bool net_fragbuf_add(net_fragbuf_t *fb, netpkt_t *pkt, uint32_t pkt_off, uint32_t frag_off, uint32_t frag_len, uint8_t more) {
    if (!fb) return false;
    if (frag_len && !pkt) return false;
    if (!fb->max_len) fb->max_len = NET_FRAGBUF_DEFAULT_MAX_LEN;
    if (!fb->step) fb->step = NET_FRAGBUF_DEFAULT_STEP;
    if (frag_off > fb->max_len || frag_len > fb->max_len - frag_off) return false;
    if (more && (frag_len & 7u)) return false;

    uint32_t need_blocks = (frag_off + frag_len + 7u) / 8u;
    if (!fb->blocks) {
        fb->block_count = (fb->max_len + 7u) / 8u;
        fb->blocks = (uint8_t*)zalloc(fb->block_count ? fb->block_count : 1u);
        if (!fb->blocks) return false;
    }
    if (need_blocks > fb->block_count) return false;

    uint32_t start = frag_off / 8u;
    for (uint32_t i = start; i < need_blocks; i++) if (fb->blocks[i]) return false;

    if (frag_len && fb->cap < frag_off + frag_len) {
        uint32_t new_cap = ((frag_off + frag_len + fb->step - 1u) / fb->step) * fb->step;
        if (new_cap < fb->step) new_cap = fb->step;
        if (new_cap > fb->max_len) new_cap = fb->max_len;

        uint8_t *new_data = (uint8_t*)zalloc(new_cap);
        if (!new_data) return false;
        if (fb->data && fb->cap) memcpy(new_data, fb->data, fb->cap);
        if (fb->data) release(fb->data);
        fb->data = new_data;
        fb->cap = new_cap;
    }

    if (frag_len && !netpkt_copyout(pkt, pkt_off, fb->data + frag_off, frag_len)) return false;
    for (uint32_t i = start; i < need_blocks; i++) fb->blocks[i] = 1;

    if (!more) {
        fb->have_last = 1;
        fb->total_len = frag_off + frag_len;
    }
    return true;
}

bool net_fragbuf_complete(const net_fragbuf_t *fb) {
    if (!fb || !fb->have_last || !fb->blocks) return false;
    uint32_t need = (fb->total_len + 7u) / 8u;
    if (need > fb->block_count) return false;
    for (uint32_t i = 0; i < need; i++) if (!fb->blocks[i]) return false;
    return true;
}

netpkt_t* net_fragbuf_take_packet(net_fragbuf_t *fb) {
    if (!net_fragbuf_complete(fb)) return NULL;
    netpkt_t *pkt = netpkt_wrap((uintptr_t)fb->data, fb->total_len, 0, fb->total_len, NULL, NULL);
    if (!pkt) return NULL;
    fb->data = NULL;
    fb->cap = 0;
    return pkt;
}
