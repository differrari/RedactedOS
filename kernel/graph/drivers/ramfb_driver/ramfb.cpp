#include "ramfb.hpp"
#include "memory/talloc.h"
#include "console/kio.h"
#include "ui/draw/draw.h"
#include "memory/memory_access.h"
#include "std/std.h"
#include "std/memory.h"
#include "theme/theme.h"

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

    fw_find_file("etc/ramfb", &file);
    
    if (file.selector == 0x0){
        kprintf("Ramfb not found");
        return false;
    }

    framebuffer_size = screen_size.width * screen_size.height * bpp;

    mem_page = palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW | MEM_DEV, false);

    framebuffer = (uint32_t*)palloc(framebuffer_size, MEM_PRIV_SHARED, MEM_RW, true);
    back_framebuffer = (uint32_t*)palloc(framebuffer_size, MEM_PRIV_SHARED, MEM_RW, true);

    ctx = {
        .dirty_rects = {},
        .fb = (uint32_t*)back_framebuffer,
        .stride = screen_size.width * bpp,
        .width = screen_size.width,
        .height = screen_size.height,
        .dirty_count = 0,
        .full_redraw = 0,
    };

    update_gpu_fb();
    
    kprintf("ramfb configured");

    return true;
}

#define cursor_dim 64
#define cursor_size cursor_dim*cursor_dim*bpp

void RamFBGPUDriver::update_gpu_fb(){
    ramfb_structure fb = {
        .addr = __builtin_bswap64((uintptr_t)framebuffer),
        .fourcc = __builtin_bswap32(RGB_FORMAT_XRGB8888),
        .flags = __builtin_bswap32(0),
        .width = __builtin_bswap32(screen_size.width),
        .height = __builtin_bswap32(screen_size.height),
        .stride = __builtin_bswap32(stride),
    };

    fw_cfg_dma_write(&fb, sizeof(fb), file.selector);
}

void RamFBGPUDriver::flush(){
    if (ctx.full_redraw) {
        uint32_t* tmp = back_framebuffer;
        back_framebuffer = framebuffer;
        framebuffer = tmp;
        update_gpu_fb();
        memcpy(back_framebuffer, framebuffer, framebuffer_size);
        ctx.fb = (uint32_t*)back_framebuffer;
        ctx.dirty_count = 0;
        ctx.full_redraw = false;
        cursor_x = 0;
        cursor_y = 0;
        cursor_updated = false;
        restore_below_cursor();
        memset(cursor_backup, 0, cursor_size);
        return;
    }
    
    volatile uint32_t* fb = (volatile uint32_t*)framebuffer;
    volatile uint32_t* bfb = (volatile uint32_t*)back_framebuffer;
    
    for (uint32_t i = 0; i < ctx.dirty_count; i++) {
        gpu_rect r = ctx.dirty_rects[i];
        
        for (uint32_t y = 0; y < r.size.height; y++) {
            uint32_t dest_y = r.point.y + y;
            if (dest_y >= screen_size.height) break;
            
            uint32_t* dst = (uint32_t*)&fb[dest_y * screen_size.width + r.point.x];
            uint32_t* src = (uint32_t*)&bfb[dest_y * screen_size.width + r.point.x];
            
            uint32_t copy_width = r.size.width;
            if (r.point.x + copy_width > screen_size.width)
                copy_width = screen_size.width - r.point.x;
            
            memcpy(dst, src, copy_width * sizeof(uint32_t));
        }
    }
    
    ctx.full_redraw = false;
    ctx.dirty_count = 0;
}

draw_ctx RamFBGPUDriver::new_cursor(uint32_t color){
    uint32_t *cursor = (uint32_t*)kalloc(mem_page, cursor_size, ALIGN_4KB, MEM_PRIV_KERNEL);
    draw_ctx cursor_ctx = {{},cursor, cursor_dim * bpp, cursor_dim, cursor_dim, 0,0};
    fb_draw_cursor(&cursor_ctx, color);
    return cursor_ctx;
}

void RamFBGPUDriver::setup_cursor(){
    cursor_backup = (uint32_t*)kalloc(mem_page, cursor_size, ALIGN_4KB, MEM_PRIV_KERNEL);
    cursor_pressed_ctx = new_cursor(CURSOR_COLOR_SELECTED);
    cursor_unpressed_ctx = new_cursor(CURSOR_COLOR_DESELECTED);
}

#define cursor_loc(x,y,row) (((y + cy) * row) + (cx + x))

void RamFBGPUDriver::restore_below_cursor(){
    for (int cy = 0; cy < cursor_dim; cy++){
        for (int cx = 0; cx < cursor_dim; cx++){
            uint32_t val = cursor_backup[cursor_loc(0, 0, cursor_dim)];
            if (val)
                framebuffer[((cursor_y + cy) * screen_size.width) + (cursor_x + cx)] = val;
        }
    }
}

void RamFBGPUDriver::update_cursor(uint32_t x, uint32_t y, bool full){
    if (x + cursor_dim >= screen_size.width || y + cursor_dim >= screen_size.height) return;
    draw_ctx cursor_ctx = cursor_pressed ? cursor_pressed_ctx : cursor_unpressed_ctx;
    if (cursor_updated){
        restore_below_cursor();
    }
    for (unsigned int cy = 0; cy < cursor_dim; cy++){
        for (unsigned int cx = 0; cx < cursor_dim; cx++){
                uint32_t val = cursor_ctx.fb[cursor_loc(0, 0, cursor_dim)];
                uint32_t screen = framebuffer[cursor_loc(x,y,screen_size.width)];
                if (val){
                    framebuffer[cursor_loc(x, y, screen_size.width)] = val;
                    cursor_backup[cursor_loc(0, 0, cursor_dim)] = screen;
                } else cursor_backup[cursor_loc(0, 0, cursor_dim)] = 0;
        }
    }
    cursor_x = x;
    cursor_y = y;
    cursor_updated = true;
}

void RamFBGPUDriver::set_cursor_pressed(bool pressed){
    cursor_pressed = pressed;
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

draw_ctx* RamFBGPUDriver::get_ctx(){
    return &ctx;
}

void RamFBGPUDriver::create_window(uint32_t x, uint32_t y, uint32_t width, uint32_t height, draw_ctx *new_ctx){
    new_ctx->fb = (uint32_t*)palloc(width * height * bpp, MEM_PRIV_SHARED, MEM_RW, true);
    new_ctx->width = width;
    new_ctx->height = height;
    new_ctx->stride = width * bpp;
}