#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "ui/graphic_types.h"
#include "std/string.h"

#define CHAR_SIZE 8

#define MAX_DIRTY_RECTS 64
#define FULL_REDRAW_THRESHOLD_PCT 35u

typedef struct draw_ctx {
    gpu_rect dirty_rects[MAX_DIRTY_RECTS];
    uint32_t* fb;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
    uint32_t dirty_count;
    bool full_redraw;
} draw_ctx;

typedef struct {
    uint32_t img_width;
    uint32_t img_height;
    uint32_t start_x;
    uint32_t start_y;
    bool flip_x;
    bool flip_y;
} image_transform;

void mark_dirty(draw_ctx *ctx, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

void fb_clear(draw_ctx *ctx, uint32_t color);
void fb_draw_pixel(draw_ctx *ctx, uint32_t x, uint32_t y, color color);
void fb_fill_rect(draw_ctx *ctx, int32_t x, int32_t y, uint32_t width, uint32_t height, color color);
void fb_fill_partial_rect(draw_ctx *ctx, uint32_t x, uint32_t y, uint32_t width, uint32_t height, color color, uint32_t start_x, uint32_t start_y, uint32_t full_width);
void fb_draw_img(draw_ctx *ctx, uint32_t x, uint32_t y, uint32_t *img, uint32_t img_width, uint32_t img_height);
void fb_draw_partial_img(draw_ctx *ctx, uint32_t *img, uint32_t x, uint32_t y, uint32_t full_width, uint32_t full_height, image_transform transform);
gpu_rect fb_draw_line(draw_ctx *ctx, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, color color);
void fb_draw_char(draw_ctx *ctx, uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color);
gpu_size fb_draw_string(draw_ctx *ctx, const char* s, uint32_t x, uint32_t y, uint32_t scale, uint32_t color);
uint32_t fb_get_char_size(uint32_t scale);
void fb_draw_cursor(draw_ctx *ctx, uint32_t color);

#ifdef __cplusplus
}
#endif