#include "fbgpu.hpp"
#include "std/std.h"
#include "console/kio.h"
#include "theme/theme.h"
#include "memory/page_allocator.h"

#define cursor_dim 64
#define cursor_size cursor_dim*cursor_dim*bpp

void FBGPUDriver::flush(){
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

draw_ctx FBGPUDriver::new_cursor(uint32_t color){
    uint32_t *cursor = (uint32_t*)kalloc(mem_page, cursor_size, ALIGN_4KB, MEM_PRIV_KERNEL);
    draw_ctx cursor_ctx = {{},cursor, cursor_dim * bpp, cursor_dim, cursor_dim, 0,0};
    fb_draw_cursor(&cursor_ctx, color);
    return cursor_ctx;
}

void FBGPUDriver::setup_cursor(){
    cursor_pressed_ctx = new_cursor(system_theme.cursor_color_selected);
    cursor_unpressed_ctx = new_cursor(system_theme.cursor_color_deselected);
}

#define cursor_loc(x,y,row) (((y + cy) * row) + (cx + x))

void FBGPUDriver::restore_below_cursor(){
    if (!ctx.full_redraw){
        mark_dirty(&ctx, cursor_x, cursor_y, cursor_dim, cursor_dim);
        flush();
    }
}

void FBGPUDriver::update_cursor(uint32_t x, uint32_t y, bool full){
    if (x + cursor_dim >= screen_size.width || y + cursor_dim >= screen_size.height) return;
    draw_ctx cursor_ctx = cursor_pressed ? cursor_pressed_ctx : cursor_unpressed_ctx;
    if (cursor_updated){
        restore_below_cursor();
    }
    for (unsigned int cy = 0; cy < cursor_dim; cy++){
        for (unsigned int cx = 0; cx < cursor_dim; cx++){
            uint32_t val = cursor_ctx.fb[cursor_loc(0, 0, cursor_dim)];
            if (val)
                framebuffer[cursor_loc(x, y, screen_size.width)] = val;
        }
    }
    cursor_x = x;
    cursor_y = y;
    cursor_updated = true;
}

void FBGPUDriver::set_cursor_pressed(bool pressed){
    cursor_pressed = pressed;
}

void FBGPUDriver::clear(color color){
    fb_clear(&ctx, color);
}

void FBGPUDriver::draw_pixel(uint32_t x, uint32_t y, color color){
    fb_draw_pixel(&ctx, x, y, color);
}

void FBGPUDriver::fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, color color){
    fb_fill_rect(&ctx, x, y, width, height, color);
}

void FBGPUDriver::draw_line(uint32_t x0, uint32_t y0, uint32_t x1,uint32_t y1, color color){
    fb_draw_line(&ctx, x0, y0, x1, y1, color);
}

void FBGPUDriver::draw_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color){
    fb_draw_char(&ctx, x, y, c, scale, color);
}

void FBGPUDriver::draw_string(string s, uint32_t x, uint32_t y, uint32_t scale, uint32_t color){
    fb_draw_string(&ctx, s.data, x, y, scale, color);
}

uint32_t FBGPUDriver::get_char_size(uint32_t scale){
    return fb_get_char_size(scale);
}

draw_ctx* FBGPUDriver::get_ctx(){
    return &ctx;
}

void FBGPUDriver::create_window(uint32_t x, uint32_t y, uint32_t width, uint32_t height, draw_ctx *new_ctx){
    new_ctx->fb = (uint32_t*)palloc(width * height * bpp, MEM_PRIV_SHARED, MEM_RW, true);
    new_ctx->width = width;
    new_ctx->height = height;
    new_ctx->stride = width * bpp;
}

void FBGPUDriver::resize_window(uint32_t width, uint32_t height, draw_ctx *win_ctx){
    size_t old_size = win_ctx->width * win_ctx->height * bpp;
    pfree(win_ctx->fb, old_size);
    create_window(0, 0, width, height, win_ctx);
}