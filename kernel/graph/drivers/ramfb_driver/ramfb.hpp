#pragma once 

#include "kstring.h"
#include "../gpu_driver.hpp"

#define MAX_DIRTY_RECTS 64

class RamFBGPUDriver : GPUDriver {
public:
    bool init(gpu_size preferred_screen_size);

    void flush();

    void clear(color color);
    void draw_pixel(uint32_t x, uint32_t y, color color);
    void draw_single_pixel(uint32_t x, uint32_t y, color color);
    void fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, color color);
    void draw_line(uint32_t x0, uint32_t y0, uint32_t x1,uint32_t y1, color color);
    void draw_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color);
    void draw_single_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color);
    gpu_size get_screen_size();
    void draw_string(kstring s, uint32_t x, uint32_t y, uint32_t scale, uint32_t color);
    uint32_t get_char_size(uint32_t scale);
    
private: 
    uintptr_t framebuffer;
    uintptr_t back_framebuffer;
    uint64_t framebuffer_size;
    gpu_size screen_size;
    gpu_rect dirty_rects[MAX_DIRTY_RECTS];
    uint32_t dirty_count = 0;
    bool full_redraw = false;
    int try_merge(gpu_rect* a, gpu_rect* b);
    void mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    uint32_t stride;
};