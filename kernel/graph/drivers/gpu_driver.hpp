#pragma once

#include "types.h"
#include "std/string.h"
#include "ui/graphic_types.h"
#include "ui/draw/draw.h"

#define bpp 4

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
    virtual void draw_string(string s, uint32_t x, uint32_t y, uint32_t scale, uint32_t color) = 0;
    virtual uint32_t get_char_size(uint32_t scale) = 0;

    virtual void setup_cursor(){};    
    virtual void update_cursor(uint32_t x, uint32_t y, bool full){};
    virtual void set_cursor_pressed(bool pressed){};

    virtual void create_window(uint32_t x, uint32_t y, uint32_t width, uint32_t height, draw_ctx *ctx) = 0;
    virtual void resize_window(uint32_t width, uint32_t height, draw_ctx *win_ctx){}
    
    virtual draw_ctx* get_ctx() = 0;

    virtual ~GPUDriver() = default;
};