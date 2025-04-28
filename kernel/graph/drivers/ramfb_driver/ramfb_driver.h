#pragma once 

#include "types.h"

bool rfb_init(uint32_t width, uint32_t height);

void rfb_flush();

void rfb_clear(uint32_t color);
void rfb_draw_pixel(uint32_t x, uint32_t y, uint32_t color);
void rfb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void rfb_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color);
void rfb_draw_char(uint32_t x, uint32_t y, char c, int scale, uint32_t color);