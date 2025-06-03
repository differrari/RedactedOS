#include "ramfb.hpp"
#include "fw/fw_cfg.h"
#include "memory/kalloc.h"
#include "console/kio.h"
#include "graph/font8x8_bridge.h"
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

    fb_set_stride(stride);
    fb_set_bounds(screen_size.width,screen_size.height);
    
    struct fw_cfg_file file;
    fw_find_file(kstring_l("etc/ramfb"), &file);
    
    if (file.selector == 0x0){
        kprintf("Ramfb not found");
        return false;
    }

    framebuffer = talloc(screen_size.width * screen_size.height * bpp);
    back_framebuffer = talloc(screen_size.width * screen_size.height * bpp);

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
    if (full_redraw) {
        memcpy((void*)framebuffer, (void*)back_framebuffer, screen_size.width * screen_size.height * bpp);
        dirty_count = 0;
        full_redraw = false;
        return;
    }
    
    volatile uint32_t* fb = (volatile uint32_t*)framebuffer;
    volatile uint32_t* bfb = (volatile uint32_t*)back_framebuffer;
    
    for (uint32_t i = 0; i < dirty_count; i++) {
        gpu_rect r = dirty_rects[i];
        
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
    
    full_redraw = false;
    dirty_count = 0;
}

void RamFBGPUDriver::clear(color color){
    fb_clear((uint32_t*)back_framebuffer, screen_size.width, screen_size.height, color);
    mark_dirty(0,0,screen_size.width,screen_size.height);
}

void RamFBGPUDriver::draw_pixel(uint32_t x, uint32_t y, color color){
    fb_draw_pixel((uint32_t*)back_framebuffer, x, y, color);
    mark_dirty(x,y,1,1);
}

void RamFBGPUDriver::fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, color color){
    fb_fill_rect((uint32_t*)back_framebuffer, x, y, width, height, color);
    mark_dirty(x,y,width,height);
}

void RamFBGPUDriver::draw_line(uint32_t x0, uint32_t y0, uint32_t x1,uint32_t y1, color color){
    gpu_rect rect = fb_draw_line((uint32_t*)framebuffer, x0, y0, x1, y1, color);
    mark_dirty(rect.point.x,rect.point.y,rect.size.width,rect.size.height);
}

void RamFBGPUDriver::draw_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color){
    fb_draw_char((uint32_t*)back_framebuffer, x, y, c, scale, color);
    mark_dirty(x,y,8*scale,8*scale);
}

gpu_size RamFBGPUDriver::get_screen_size(){
    return screen_size;
}

void RamFBGPUDriver::draw_string(string s, uint32_t x, uint32_t y, uint32_t scale, uint32_t color){
    gpu_size drawn_string = fb_draw_string((uint32_t*)back_framebuffer, s, x, y, scale, color);
    mark_dirty(x,y,drawn_string.width,drawn_string.height);
}

uint32_t RamFBGPUDriver::get_char_size(uint32_t scale){
    return fb_get_char_size(scale);
}

int RamFBGPUDriver::try_merge(gpu_rect* a, gpu_rect* b) {
    uint32_t ax2 = a->point.x + a->size.width;
    uint32_t ay2 = a->point.y + a->size.height;
    uint32_t bx2 = b->point.x + b->size.width;
    uint32_t by2 = b->point.y + b->size.height;

    if (a->point.x > bx2 || b->point.x > ax2 || a->point.y > by2 || b->point.y > ay2)
        return 0;

    uint32_t min_x = a->point.x < b->point.x ? a->point.x : b->point.x;
    uint32_t min_y = a->point.y < b->point.y ? a->point.y : b->point.y;
    uint32_t max_x = ax2 > bx2 ? ax2 : bx2;
    uint32_t max_y = ay2 > by2 ? ay2 : by2;

    a->point.x = min_x;
    a->point.y = min_y;
    a->size.width = max_x - min_x;
    a->size.height = max_y - min_y;

    return 1;
}

void RamFBGPUDriver::mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    gpu_rect new_rect = { x, y, w, h };

    for (uint32_t i = 0; i < dirty_count; i++) {
        if (try_merge(&dirty_rects[i], &new_rect))
            return;
    }

    if (dirty_count < MAX_DIRTY_RECTS_RFB)
        dirty_rects[dirty_count++] = new_rect;
    else
        full_redraw = true;
}