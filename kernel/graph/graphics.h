#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "ui/graphic_types.h"
#include "std/string.h"
#include "dev/driver_base.h"

bool gpu_ready();

void gpu_flush();

void gpu_clear(color color);
void gpu_draw_pixel(gpu_point p, color color);
void gpu_fill_rect(gpu_rect r, color color);
void gpu_draw_line(gpu_point p0, gpu_point p1, color color);
void gpu_draw_char(gpu_point p, char c, uint32_t scale, uint32_t color);
gpu_size gpu_get_screen_size();
void gpu_draw_string(string s, gpu_point p, uint32_t scale, uint32_t color);
uint32_t gpu_get_char_size(uint32_t scale);

extern driver_module graphics_module;

#ifdef __cplusplus
}
#endif