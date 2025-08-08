#include "videocore.hpp"
#include "fw/fw_cfg.h"
#include "memory/talloc.h"
#include "console/kio.h"
#include "ui/draw/draw.h"
#include "memory/memory_access.h"
#include "std/std.hpp"
#include "std/memfunctions.h"
#include "mailbox/mailbox.h"
#include "math/math.h"
#include "memory/mmu.h"

#define RGB_FORMAT_XRGB8888 ((uint32_t)('X') | ((uint32_t)('R') << 8) | ((uint32_t)('2') << 16) | ((uint32_t)('4') << 24))

#define BUS_ADDRESS(addr)   ((addr) & ~0xC0000000)

VideoCoreGPUDriver* VideoCoreGPUDriver::try_init(gpu_size preferred_screen_size){
    VideoCoreGPUDriver* driver = new VideoCoreGPUDriver();
    if (driver->init(preferred_screen_size))
        return driver;
    delete driver;
    return nullptr;
}

volatile uint32_t rmbox[40] __attribute__((aligned(16))) = {
    30 * 4,// Buf size
    0,// Request. Code 0
    MBOX_VC_PHYS_SIZE_TAG, 8, 0, 0, 0,// Physical size
    MBOX_VC_VIRT_SIZE_TAG, 8, 0, 0, 0,// Virtual size
    MBOX_VC_DEPTH_TAG | MBOX_SET_VALUE, 4, 4, 32,// Depth
    MBOX_VC_PITCH_TAG, 4, 0, 0,//Pitch
    MBOX_VC_FORMAT_TAG | MBOX_SET_VALUE, 4, 4, 0, //BGR
    MBOX_VC_FRAMEBUFFER_TAG, 8, 0, 16, 0,
    0,// End
};

bool VideoCoreGPUDriver::init(gpu_size preferred_screen_size){
    kprintf("Initializing VideoCore GPU");

    if (!mailbox_call(rmbox, 8)) {
        kprintf("Failed videocore setup");
        return false;
    }
    uint32_t phys_w = rmbox[5];
    uint32_t phys_h = rmbox[6];
    uint32_t virt_w = rmbox[10];
    uint32_t virt_h = rmbox[11];
    uint32_t depth  = rmbox[15];
    stride = rmbox[19];

    bpp = depth/8;
    
    screen_size = (gpu_size){phys_w,phys_h};
    kprintf("Size %ix%i (%ix%i) (%ix%i) | %i (%i)",phys_w,phys_h,virt_w,virt_h,screen_size.width,screen_size.height,depth, stride);
    
    fb_set_stride(stride);
    fb_set_bounds(screen_size.width,screen_size.height);

    framebuffer = rmbox[27];
    size_t fb_size = rmbox[28];
    mem_page = palloc(0x1000, true, true, false);
    back_framebuffer = (uintptr_t)kalloc(mem_page, fb_size, ALIGN_16B, true, true);
    kprintf("Framebuffer allocated to %x (%i). BPP %i. Stride %i",framebuffer, fb_size, bpp, stride/bpp);
    mark_used(framebuffer,count_pages(fb_size,PAGE_SIZE));
    for (size_t i = framebuffer; i < framebuffer + fb_size; i += GRANULE_4KB)
        register_device_memory(i,i);
    return true;
}

void VideoCoreGPUDriver::flush(){
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
            if (r.point.x + copy_width > screen_size.width) copy_width = screen_size.width - r.point.x;
            
            memcpy(dst, src, copy_width * sizeof(uint32_t));
        }
    }
    
    full_redraw = false;
    dirty_count = 0;
}

void VideoCoreGPUDriver::clear(color color){
    fb_clear((uint32_t*)back_framebuffer, color);
}

void VideoCoreGPUDriver::draw_pixel(uint32_t x, uint32_t y, color color){
    fb_draw_pixel((uint32_t*)back_framebuffer, x, y, color);
    mark_dirty(x,y,1,1);
}

void VideoCoreGPUDriver::fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, color color){
    fb_fill_rect((uint32_t*)back_framebuffer, x, y, width, height, color);
}

void VideoCoreGPUDriver::draw_line(uint32_t x0, uint32_t y0, uint32_t x1,uint32_t y1, color color){
    fb_draw_line((uint32_t*)back_framebuffer, x0, y0, x1, y1, color);
}

void VideoCoreGPUDriver::draw_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color){
    fb_draw_char((uint32_t*)back_framebuffer, x, y, c, scale, color);
}

gpu_size VideoCoreGPUDriver::get_screen_size(){
    return screen_size;
}

void VideoCoreGPUDriver::draw_string(string s, uint32_t x, uint32_t y, uint32_t scale, uint32_t color){
    fb_draw_string((uint32_t*)back_framebuffer, s, x, y, scale, color);
}

uint32_t VideoCoreGPUDriver::get_char_size(uint32_t scale){
    return fb_get_char_size(max(1,scale-1));//TODO: Screen resolution seems fixed at 640x480 (on QEMU at least). So we make the font smaller
}
