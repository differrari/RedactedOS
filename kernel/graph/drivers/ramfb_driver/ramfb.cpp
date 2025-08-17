#include "ramfb.hpp"
#include "fw/fw_cfg.h"
#include "memory/talloc.h"
#include "console/kio.h"
#include "ui/draw/draw.h"
#include "memory/memory_access.h"
#include "std/std.hpp"
#include "std/memfunctions.h"

typedef struct {
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
}__attribute__((packed)) ramfb_structure;

#define RGB_FORMAT_XRGB8888 ((uint32_t)('X') | ((uint32_t)('R') << 8) | ((uint32_t)('2') << 16) | ((uint32_t)('4') << 24))

#define bpp 4

RamFBGPUDriver* RamFBGPUDriver::try_init(gpu_size preferred_screen_size){
    RamFBGPUDriver* driver = new RamFBGPUDriver();
    if (driver->init(preferred_screen_size))
        return driver;
    delete driver;
    return nullptr;
}

bool RamFBGPUDriver::init(gpu_size preferred_screen_size){
    screen_size = preferred_screen_size;

    stride = bpp * screen_size.width;

    ctx = {
        .fb = (uint32_t*)framebuffer,
        .stride = screen_size.width * bpp,
        .width = screen_size.width,
        .height = screen_size.height
    };
    
    struct fw_cfg_file file;
    fw_find_file("etc/ramfb", &file);
    
    if (file.selector == 0x0){
        kprintf("Ramfb not found");
        return false;
    }

    size_t fb_size = screen_size.width * screen_size.height * bpp;

    mem_page = palloc(0x1000, true, true, false);

    framebuffer = (uintptr_t)kalloc(mem_page, fb_size, true, true, false);
    back_framebuffer = (uintptr_t)kalloc(mem_page, fb_size, true, true, false);

    ramfb_structure fb = {
        .addr = __builtin_bswap64(framebuffer),
        .fourcc = __builtin_bswap32(RGB_FORMAT_XRGB8888),
        .flags = __builtin_bswap32(0),
        .width = __builtin_bswap32(screen_size.width),
        .height = __builtin_bswap32(screen_size.height),
        .stride = __builtin_bswap32(stride),
    };

    fw_cfg_dma_write(&fb, sizeof(fb), file.selector);
    
    kprintf("ramfb configured");

    return true;
}

void RamFBGPUDriver::flush(){
    if (ctx.full_redraw) {
        memcpy((void*)framebuffer, (void*)back_framebuffer, screen_size.width * screen_size.height * bpp);
        ctx.dirty_count = 0;
        ctx.full_redraw = false;
        return;
    }
    
    volatile uint32_t* fb = (volatile uint32_t*)framebuffer;
    volatile uint32_t* bfb = (volatile uint32_t*)back_framebuffer;
    
    for (uint32_t i = 0; i < ctx.dirty_count; i++) {
        gpu_rect r = ctx.dirty_rects[i];
        
        for (uint32_t y = 0; y < r.size.height; y++) {
            uint32_t dest_y = r.point.y + y;
            if (dest_y >= screen_size.height) break;
            
            uint32_t* dst = (uint32_t*)&fb[dest_y * (stride / 4) + r.point.x];
            uint32_t* src = (uint32_t*)&bfb[dest_y * (stride / 4) + r.point.x];
            
            uint32_t copy_width = r.size.width;
            if (r.point.x + copy_width > screen_size.width)
                copy_width = screen_size.width - r.point.x;
            
            memcpy(dst, src, copy_width * sizeof(uint32_t));
        }
    }
    
    ctx.full_redraw = false;
    ctx.dirty_count = 0;
}

void RamFBGPUDriver::clear(color color){
    fb_clear(&ctx, color);
}

void RamFBGPUDriver::draw_pixel(uint32_t x, uint32_t y, color color){
    fb_draw_pixel(&ctx, x, y, color);
}

void RamFBGPUDriver::fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, color color){
    fb_fill_rect(&ctx, x, y, width, height, color);
}

void RamFBGPUDriver::draw_line(uint32_t x0, uint32_t y0, uint32_t x1,uint32_t y1, color color){
    fb_draw_line(&ctx, x0, y0, x1, y1, color);
}

void RamFBGPUDriver::draw_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color){
    fb_draw_char(&ctx, x, y, c, scale, color);
}

gpu_size RamFBGPUDriver::get_screen_size(){
    return screen_size;
}

void RamFBGPUDriver::draw_string(string s, uint32_t x, uint32_t y, uint32_t scale, uint32_t color){
    fb_draw_string(&ctx, s.data, x, y, scale, color);
}

uint32_t RamFBGPUDriver::get_char_size(uint32_t scale){
    return fb_get_char_size(scale);
}

draw_ctx RamFBGPUDriver::get_ctx(){
    return ctx;
}