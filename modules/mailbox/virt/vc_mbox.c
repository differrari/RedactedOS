#include "mailbox/vc_mbox.h"
#include "mailbox/mailbox.h"

static volatile uint32_t mbox_buf[128] __attribute__((aligned(16)));

bool mbox_parse_tag(volatile uint32_t* buf, uint32_t tag, uint32_t* out, uint32_t words) {
    return false;
}

bool mbox_query_modes(uint32_t* phys_w, uint32_t* phys_h, uint32_t* virt_w, uint32_t* virt_h, uint32_t* depth, uint32_t* pitch, uint32_t pixel_format) {
    return false;
}

bool mbox_alloc_fb(uint32_t virt_w, uint32_t virt_h, uint32_t* fb_bus_addr, uint32_t* fb_size) {
    return false;
}

bool mbox_set_offset(uint32_t yoff) {
    return false;
}