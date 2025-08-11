#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "ui/graphic_types.h"
#include "std/string.h"

#define CHAR_SIZE 8

int try_merge(gpu_rect* a, gpu_rect* b);
void mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

//TODO: to make this a more robust drawing library available without syscall, unify dirty_rects, fb pointer, stride and size into a context structure

void fb_clear(uint32_t* fb, uint32_t color);
void fb_draw_pixel(uint32_t* fb, uint32_t x, uint32_t y, color color);
void fb_fill_rect(uint32_t* fb, uint32_t x, uint32_t y, uint32_t width, uint32_t height, color color);
gpu_rect fb_draw_line(uint32_t* fb, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, color color);
void fb_draw_char(uint32_t* fb, uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color);
gpu_size fb_draw_string(uint32_t* fb, string s, uint32_t x, uint32_t y, uint32_t scale, uint32_t color);
uint32_t fb_get_char_size(uint32_t scale);

void fb_set_stride(uint32_t new_stride);
void fb_set_bounds(uint32_t width, uint32_t height);

#define MAX_DIRTY_RECTS 64

extern gpu_rect dirty_rects[MAX_DIRTY_RECTS];
extern uint32_t dirty_count;
extern bool full_redraw;

#ifdef __cplusplus
}
#endif