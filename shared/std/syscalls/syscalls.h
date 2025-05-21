#pragma once

#include "types.h"
#include "ui/graphic_types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern void printf_args(const char *fmt, const uint64_t *args, uint32_t arg_count);

extern uintptr_t malloc(size_t size);

extern void free(void *ptr, size_t size);

extern void clear_screen(color color);

extern void gpu_flush_data();

extern void sleep(uint64_t time);

gpu_size* gpu_screen_size();
uint32_t gpu_char_size(uint32_t scale);

extern void draw_primitive_pixel(gpu_point *p, color color);
extern void draw_primitive_line(gpu_point *p0, gpu_point *p1, color color);
extern void draw_primitive_rect(gpu_rect *r, color color);
extern void draw_primitive_char(gpu_point *p, char c, uint32_t scale, uint32_t color);
extern void draw_primitive_text(const char *text, gpu_point *p, uint32_t scale, uint32_t color);

#define printf(fmt, ...) \
    ({ \
        uint64_t _args[] = { __VA_ARGS__ }; \
        printf_args((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
    })

#ifdef __cplusplus
}
#endif