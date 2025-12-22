#include "mailbox/vc_mbox.h"
#include "mailbox/mailbox.h"

#define MBOX_CH_PROP 8
#define TAG_ALLOCATE_BUFFER 0x00040001
#define TAG_SET_PHYS_WH 0x00048003
#define TAG_GET_PHYS_WH 0x00040003
#define TAG_SET_VIRT_WH 0x00048004
#define TAG_GET_VIRT_WH 0x00040004
#define TAG_SET_DEPTH 0x00048005
#define TAG_GET_DEPTH 0x00040005
#define TAG_SET_PIXEL_ORDER 0x00048006
#define TAG_GET_PITCH 0x00040008
#define TAG_SET_VIRT_OFFSET 0x00048009

#define REQ_CODE 0x00000000
#define RESP_OK 0x80000000
#define END_TAG 0x00000000

static volatile uint32_t mbox_buf[128] __attribute__((aligned(16)));

bool mbox_parse_tag(volatile uint32_t* buf, uint32_t tag, uint32_t* out, uint32_t words) {
    uint32_t idx = 2, total = buf[0] / 4;
    while (idx + 3 < total)
    {
        uint32_t t = buf[idx + 0];
        uint32_t vlen = buf[idx + 1] / 4;
        uint32_t ro = buf[idx + 2];
        if (t == END_TAG) break;
        if (t == tag && (ro & RESP_OK))
        {
            if (words > vlen) return false;
            for (uint32_t i = 0; i < words; ++i) out[i] = buf[idx + 3 + i];

            return true;
        }
        idx += 3 + vlen;
    }
    return false;
}

bool mbox_query_modes(uint32_t* phys_w, uint32_t* phys_h, uint32_t* virt_w, uint32_t* virt_h, uint32_t* depth, uint32_t* pitch, uint32_t pixel_format) {
    volatile uint32_t *b = mbox_buf;
    uint32_t i = 2;

    b[0] = 0;
    b[1] = REQ_CODE;

    uint32_t pixel_order;
    switch (pixel_format) {
    case (uint32_t)('X') | ('R' << 8) | ('2' << 16) | ('4' << 24):
    case (uint32_t)('A') | ('R' << 8) | ('2' << 16) | ('4' << 24):
    case (uint32_t)('R') | ('G' << 8) | ('2' << 16) | ('4' << 24):
        pixel_order = 0;
        break;
    case (uint32_t)('X') | ('B' << 8) | ('2' << 16) | ('4' << 24):
    case (uint32_t)('A') | ('B' << 8) | ('2' << 16) | ('4' << 24):
    case (uint32_t)('B') | ('G' << 8) | ('2' << 16) | ('4' << 24):
        pixel_order = 1;
        break;
    default:
        pixel_order = 0;
        break;
    }

    b[i++] = TAG_SET_PIXEL_ORDER;
    b[i++] = 4;
    b[i++] = REQ_CODE;
    b[i++] = pixel_order;

    b[i++] = TAG_SET_DEPTH;
    b[i++] = 4;
    b[i++] = REQ_CODE;
    b[i++] = 32;

    b[i++] = TAG_GET_PHYS_WH;
    b[i++] = 8;
    b[i++] = REQ_CODE;
    b[i++] = 0;
    b[i++] = 0;
    b[i++] = TAG_GET_VIRT_WH;
    b[i++] = 8;
    b[i++] = REQ_CODE;
    b[i++] = 0;
    b[i++] = 0;
    b[i++] = TAG_GET_DEPTH;
    b[i++] = 4;
    b[i++] = REQ_CODE;
    b[i++] = 0;
    b[i++] = TAG_GET_PITCH;
    b[i++] = 4;
    b[i++] = REQ_CODE;
    b[i++] = 0;
    b[i++] = END_TAG;
    b[0] = i * 4;
    b[1] = REQ_CODE;
    if (!mailbox_call(b, MBOX_CH_PROP)) return false;
    if (!(b[1] & RESP_OK)) return false;

    uint32_t tmp[2];
    if (!mbox_parse_tag(b, TAG_GET_PHYS_WH, tmp, 2)) return false;
    *phys_w = tmp[0];
    *phys_h = tmp[1];

    if (!mbox_parse_tag(b, TAG_GET_VIRT_WH, tmp, 2)) return false;
    *virt_w = tmp[0];
    *virt_h = tmp[1];

    if (!mbox_parse_tag(b, TAG_GET_DEPTH, tmp, 1)) return false;
    *depth = tmp[0];
    if (!mbox_parse_tag(b, TAG_GET_PITCH, tmp, 1)) return false;
    *pitch = tmp[0];
    return true;
}

bool mbox_alloc_fb(uint32_t virt_w, uint32_t virt_h, uint32_t* fb_bus_addr, uint32_t* fb_size) {
    volatile uint32_t *b = mbox_buf;
    uint32_t i = 2;

    b[0] = 0;
    b[1] = REQ_CODE;

    b[i++] = TAG_SET_VIRT_WH;
    b[i++] = 8;
    b[i++] = REQ_CODE;
    b[i++] = virt_w;
    b[i++] = virt_h;

    b[i++] = TAG_ALLOCATE_BUFFER;
    b[i++] = 8;
    b[i++] = REQ_CODE;
    b[i++] = 16;
    b[i++] = 0;

    b[i++] = END_TAG;
    b[0] = i * 4;
    b[1] = REQ_CODE;
    if (!mailbox_call(b, MBOX_CH_PROP)) return false;
    if (!(b[1] & RESP_OK)) return false;
    uint32_t tmp[2];
    if (!mbox_parse_tag(b, TAG_ALLOCATE_BUFFER, tmp, 2)) return false;
    *fb_bus_addr = tmp[0];
    *fb_size = tmp[1];
    return true;
}

bool mbox_set_offset(uint32_t yoff) {
    volatile uint32_t *b = mbox_buf;
    uint32_t i = 2;
    b[0] = 0;
    b[1] = REQ_CODE;
    b[i++] = TAG_SET_VIRT_OFFSET;
    b[i++] = 8;
    b[i++] = REQ_CODE;
    b[i++] = 0;
    b[i++] = yoff;
    b[i++] = END_TAG;
    b[0] = i * 4;
    b[1] = REQ_CODE;
    if (!mailbox_call(b, MBOX_CH_PROP)) return false;
    if (!(b[1] & RESP_OK)) return false;
    return true;
}