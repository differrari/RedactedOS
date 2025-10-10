#include "vc_mbox.h"
#include "mailbox/mailbox.h"

bool mbox_parse_tag(volatile uint32_t* buf, uint32_t tag, uint32_t* out, uint32_t words) {
    uint32_t i = 2;
    for (;;) {
        uint32_t t = buf[i];
        if (t == 0) break;

        uint32_t sz = buf[i + 1];
        uint32_t len = buf[i + 2];
        if (t == tag) {
            uint32_t to_copy = len >> 2;
            if (to_copy > words) to_copy = words;
            for (uint32_t j = 0; j < to_copy; ++j) out[j] = buf[i + 3 + j];

            return true;
        }
        i += 3 + ((sz + 3) >> 2);
    }
    return false;
}

bool mbox_query_modes(uint32_t* phys_w, uint32_t* phys_h, uint32_t* virt_w, uint32_t* virt_h, uint32_t* depth, uint32_t* pitch, uint32_t pixel_format) {
    static volatile uint32_t mb[64] __attribute__((aligned(16)));
    uint32_t words = 2 + 3 + 3 + 3 + 3 + 3 + 1;
    uint32_t bytes = words << 2;
    uint32_t aligned = (bytes + 15u) & ~15u;

    mb[0] = aligned;
    mb[1] = 0;

    mb[2] = MBOX_VC_PHYS_SIZE_TAG;
    mb[3] = 8;
    mb[4] = 0;
    mb[5] = 0;
    mb[6] = 0;

    mb[7] = MBOX_VC_VIRT_SIZE_TAG;
    mb[8] = 8;
    mb[9] = 0;
    mb[10] = 0;
    mb[11] = 0;

    mb[12] = MBOX_VC_DEPTH_TAG | MBOX_SET_VALUE;
    mb[13] = 4;
    mb[14] = 4;
    mb[15] = 32;

    mb[16] = MBOX_VC_PITCH_TAG;
    mb[17] = 4;
    mb[18] = 0;
    mb[19] = 0;

    mb[20] = MBOX_VC_FORMAT_TAG | MBOX_SET_VALUE;
    mb[21] = 4;
    mb[22] = 4;
    mb[23] = pixel_format;

    mb[24] = 0;

    if (!mailbox_call(mb, 8)) return false;

    uint32_t tmp2[2];
    if (!mbox_parse_tag(mb, MBOX_VC_PHYS_SIZE_TAG, tmp2, 2)) return false;
    *phys_w = tmp2[0];
    *phys_h = tmp2[1];

    if (!mbox_parse_tag(mb, MBOX_VC_VIRT_SIZE_TAG, tmp2, 2)) return false;
    *virt_w = tmp2[0];
    *virt_h = tmp2[1];

    uint32_t tmp1[1];
    if (!mbox_parse_tag(mb, MBOX_VC_DEPTH_TAG, tmp1, 1)) return false;
    *depth = tmp1[0];

    if (!mbox_parse_tag(mb, MBOX_VC_PITCH_TAG, tmp1, 1)) return false;
    *pitch = tmp1[0];

    return true;
}

bool mbox_alloc_fb(uint32_t virt_w, uint32_t virt_h, uint32_t* fb_addr, uint32_t* fb_size) {
    static volatile uint32_t mb[64] __attribute__((aligned(16)));
    uint32_t words = 2 + 3 + 3 + 1;
    uint32_t bytes = words << 2;
    uint32_t aligned = (bytes + 15u) & ~15u;

    mb[0] = aligned;
    mb[1] = 0;

    mb[2] = MBOX_VC_VIRT_SIZE_TAG | MBOX_SET_VALUE;
    mb[3] = 8;
    mb[4] = 8;
    mb[5] = virt_w;
    mb[6] = virt_h;

    mb[7] = MBOX_VC_FRAMEBUFFER_TAG;
    mb[8] = 8;
    mb[9] = 0;
    mb[10] = 16;
    mb[11] = 0;

    mb[12] = 0;

    if (!mailbox_call(mb, 8)) return false;

    uint32_t tmp2[2];
    if (!mbox_parse_tag(mb, MBOX_VC_FRAMEBUFFER_TAG, tmp2, 2)) return false;
    *fb_addr = tmp2[0];
    *fb_size = tmp2[1];

    return true;
}

bool mbox_set_offset(uint32_t yoff) {
    static volatile uint32_t mb[16] __attribute__((aligned(16))) = {30 * 4, 0, MBOX_VC_OFFSET_TAG | MBOX_SET_VALUE, 8, 8, 0, 0, 0,};
    mb[6] = yoff;
    return mailbox_call(mb, 8);
}