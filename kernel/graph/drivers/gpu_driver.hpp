#pragma once

#include "types.h"
#include "ui/graphic_types.h"

class GPUDriver {
public:
    GPUDriver(){}
    virtual bool init(gpu_size preferred_screen_size) = 0;

    virtual void flush() = 0;

    virtual void clear(color color) = 0;
    virtual void draw_pixel(uint32_t x, uint32_t y, color color) = 0;
    virtual void fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, color color) = 0;
    virtual void draw_line(uint32_t x0, uint32_t y0, uint32_t x1,uint32_t y1, color color) = 0;
    virtual void draw_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color) = 0;
    virtual gpu_size get_screen_size() = 0;
    virtual void draw_string(kstring s, uint32_t x, uint32_t y, uint32_t scale, uint32_t color) = 0;
    virtual uint32_t get_char_size(uint32_t scale) = 0;

    ~GPUDriver() = default;
};