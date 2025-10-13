#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool mbox_parse_tag(volatile uint32_t* buf, uint32_t tag, uint32_t* out, uint32_t words);
bool mbox_query_modes(uint32_t* phys_w, uint32_t* phys_h, uint32_t* virt_w, uint32_t* virt_h, uint32_t* depth, uint32_t* pitch, uint32_t pixel_format);
bool mbox_alloc_fb(uint32_t virt_w, uint32_t virt_h, uint32_t* fb_bus_addr, uint32_t* fb_size);
bool mbox_set_offset(uint32_t yoff);

#ifdef __cplusplus
}
#endif