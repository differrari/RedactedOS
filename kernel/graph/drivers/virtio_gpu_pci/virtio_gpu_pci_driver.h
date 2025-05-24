#pragma once 

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "ui/graphic_types.h"
#include "kstring.h"

bool vgp_init(uint32_t width, uint32_t height);
void vgp_flush();
void vgp_clear(uint32_t color);
void vgp_draw_pixel(uint32_t x, uint32_t y, uint32_t color);
void vgp_draw_single_pixel(uint32_t x, uint32_t y, uint32_t color);
void vgp_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void vgp_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color);
void vgp_draw_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color);
void vgp_draw_single_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color);
void vgp_draw_string(kstring s, uint32_t x, uint32_t y, uint32_t scale, uint32_t color);
uint32_t vgp_get_char_size(uint32_t scale);
gpu_size vgp_get_screen_size();
#ifdef __cplusplus
}
#endif
