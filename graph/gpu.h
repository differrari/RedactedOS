#pragma once

#include "types.h"
#include "graph/gpu_types.h"

void gpu_init(size preferred_screen_size);

void gpu_flush();

void gpu_clear(color color);
void gpu_draw_pixel(point p, color color);
void gpu_fill_rect(rect r, color color);
void gpu_draw_line(point p0, point p1, color color);
void gpu_draw_char(point p, char c, uint32_t color);