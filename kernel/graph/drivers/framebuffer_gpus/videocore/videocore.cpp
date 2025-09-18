#include "videocore.hpp"
#include "fw/fw_cfg.h"
#include "memory/talloc.h"
#include "console/kio.h"
#include "ui/draw/draw.h"
#include "memory/memory_access.h"
#include "std/std.h"
#include "std/memory.h"
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
    kprintf("[VIDEOCORE] Initializing GPU");

    if (!mailbox_call(rmbox, 8)) {
        kprintf("[VIDEOCORE] Failed mailbox setup");
        return false;
    }
    uint32_t phys_w = rmbox[5];
    uint32_t phys_h = rmbox[6];
    uint32_t virt_w = rmbox[10];
    uint32_t virt_h = rmbox[11];
    uint32_t depth  = rmbox[15];
    stride = rmbox[19];

    if (bpp != depth/8){
        kprintf("[VIDEOCORE] failed to initialize. Wrong BPP (%i), should be %i. Check your config.txt file",depth/8, bpp);
        return false;
    }
    
    screen_size = (gpu_size){phys_w,phys_h};
    kprintf("[VIDEOCORE] Size %ix%i (%ix%i) (%ix%i) | %i (%i)",phys_w,phys_h,virt_w,virt_h,screen_size.width,screen_size.height,depth, stride);

    framebuffer = (uint32_t*)(uintptr_t)rmbox[27];
    size_t fb_size = rmbox[28];
    mem_page = palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW | MEM_DEV, false);
    back_framebuffer = (uint32_t*)palloc(framebuffer_size, MEM_PRIV_SHARED, MEM_RW, true);
    //TODO: make backbuffer just framebuffer + bf_size/2
    kprintf("[VIDEOCORE] Framebuffer allocated to %x (%i). BPP %i. Stride %i",framebuffer, fb_size, bpp, stride/bpp);
    mark_used((uintptr_t)framebuffer,count_pages(fb_size,PAGE_SIZE));
    for (size_t i = (uintptr_t)framebuffer; i < (uintptr_t)framebuffer + fb_size; i += GRANULE_4KB)
        register_device_memory(i,i);

    ctx = {
        .dirty_rects = {},
        .fb = (uint32_t*)back_framebuffer,
        .stride = screen_size.width * bpp,
        .width = screen_size.width,
        .height = screen_size.height,
        .dirty_count = 0,
        .full_redraw = 0,
    };

    return true;
}

void VideoCoreGPUDriver::update_gpu_fb(){
    // memcpy(void *dest, const void *src, uint64_t count)
    //TODO: set fb offset to fb_size/2
}

gpu_size VideoCoreGPUDriver::get_screen_size(){
    return screen_size;
}