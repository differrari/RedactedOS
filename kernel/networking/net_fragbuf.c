#include "networking/net_fragbuf.h"
#include "alloc/allocate.h"
#include "std/memory.h"

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
    if (frag_len) {
        if (!pkt) return false;
        uint32_t pkt_len = netpkt_len(pkt);
        if (pkt_off > pkt_len || frag_len > pkt_len - pkt_off) return false;
    }
    if (frag_off & 7u) return false;
    if (!fb->max_len) fb->max_len = NET_FRAGBUF_DEFAULT_MAX_LEN;
    if (!fb->step) fb->step = NET_FRAGBUF_DEFAULT_STEP;
    if (frag_off > fb->max_len || frag_len > fb->max_len - frag_off) return false;
    if (more && (frag_len & 7u)) return false;
    uint32_t frag_end = frag_off + frag_len;
    if (fb->have_last && frag_end > fb->total_len) return false;
    if (!more && fb->have_last && frag_end != fb->total_len) return false;
    if (!more && fb->max_end > frag_end) return false;

    uint32_t need_blocks = (frag_end + 7u) / 8u;
    if (!fb->blocks) {
        fb->block_count = (fb->max_len + 7u) / 8u;
        uint32_t bitmap_bytes = (fb->block_count + 7u) / 8u;
        fb->blocks = (uint8_t*)zalloc(bitmap_bytes ? bitmap_bytes : 1u);
        if (!fb->blocks) return false;
    }
    if (need_blocks > fb->block_count) return false;

    uint32_t start = frag_off / 8u;
    uint32_t first_byte = start / 8u;
    uint32_t end_byte = (need_blocks + 7u) / 8u;
    uint8_t first_mask = (uint8_t)(0xFF << (start & 7u));
    uint8_t last_mask = (need_blocks & 7u) ? (uint8_t)((1u << (need_blocks & 7u)) - 1) : 0xFF;
    if (first_byte < end_byte) {
        uint8_t mask = first_byte + 1 == end_byte ? first_mask & last_mask : first_mask;
        if (fb->blocks[first_byte] & mask) return false;
        for (uint32_t i = first_byte + 1; i + 1 < end_byte; i++) if (fb->blocks[i]) return false;
        if (first_byte + 1 < end_byte && (fb->blocks[end_byte-1] & last_mask)) return false;
    }

    if (frag_len && fb->cap < frag_off + frag_len) {
        uint32_t new_cap = ((frag_off + frag_len + fb->step - 1u) / fb->step) * fb->step;
        if (new_cap < fb->step) new_cap = fb->step;
        if (new_cap > fb->max_len) new_cap = fb->max_len;

        uint8_t *new_data = fb->data ? (uint8_t*)reallocate(fb->data, new_cap) : (uint8_t*)zalloc(new_cap);
        if (!new_data) return false;
        fb->data = new_data;
        fb->cap = new_cap;
    }

    if (frag_len && !netpkt_copyout(pkt, pkt_off, fb->data + frag_off, frag_len)) return false;
    if (first_byte < end_byte) {
        if (first_byte+1 == end_byte) fb->blocks[first_byte] |= (first_mask & last_mask); 
        else {
            fb->blocks[first_byte] |= first_mask;
            if (end_byte > first_byte + 2) memset(fb->blocks + first_byte + 1, 0xFF, end_byte - first_byte - 2);
            fb->blocks[end_byte - 1] |= last_mask;
        }
    }
    fb->filled_blocks += need_blocks - start;
    if (frag_end > fb->max_end) fb->max_end = frag_end;

    if (!more) {
        fb->have_last = 1;
        fb->total_len = frag_end;
    }
    return true;
}

bool net_fragbuf_complete(const net_fragbuf_t *fb) {
    if (!fb || !fb->have_last || !fb->blocks) return false;
    uint32_t need = (fb->total_len + 7u) / 8u;
    if (need > fb->block_count) return false;
    return fb->filled_blocks == need;
}

netpkt_t* net_fragbuf_take_packet(net_fragbuf_t *fb) {
    if (!net_fragbuf_complete(fb)) return NULL;
    netpkt_t *pkt = netpkt_wrap((uintptr_t)fb->data, fb->total_len, 0, fb->total_len, NULL, NULL);
    if (!pkt) return NULL;
    fb->data = NULL;
    fb->cap = 0;
    return pkt;
}
