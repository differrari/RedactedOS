#include "videocore.hpp"
#include "fw/fw_cfg.h"
#include "memory/talloc.h"
#include "console/kio.h"
#include "ui/draw/draw.h"
#include "std/memory_access.h"
#include "std/std.h"
#include "std/memory.h"
#include "mailbox/mailbox.h"
#include "math/math.h"
#include "memory/mmu.h"
#include "memory/page_allocator.h"
#include "mailbox/vc_mbox.h"

static constexpr uint32_t RGB_FORMAT_XRGB8888 = ((uint32_t)('X') | ((uint32_t)('R') << 8) | ((uint32_t)('2') << 16) | ((uint32_t)('4') << 24));

#define BUS_ADDRESS(addr)   ((addr) & ~0xC0000000)

VideoCoreGPUDriver* VideoCoreGPUDriver::try_init(gpu_size preferred_screen_size){
    VideoCoreGPUDriver* driver = new VideoCoreGPUDriver();
    if (driver->init(preferred_screen_size))
        return driver;
    delete driver;
    return nullptr;
}

bool VideoCoreGPUDriver::init(gpu_size preferred_screen_size){
    kprintf("[VIDEOCORE] Initializing GPU");

    uint32_t phys_w, phys_h, virt_w, virt_h, depth, pitch;
    if (!mbox_query_modes(&phys_w, &phys_h, &virt_w, &virt_h, &depth, &pitch, RGB_FORMAT_XRGB8888)) {
        kprintf("[VIDEOCORE] Failed mailbox setup");
        return false;
    }

    if (depth != bpp*8){
        kprintf("[VIDEOCORE] failed to initialize. Wrong BPP (%i), should be %i. Check your config.txt file",depth/8, bpp);
        return false;
    }

    if (pitch == 0) {
        kprintf("[VIDEOCORE] Invalid pitch");
        return false;
    }

    stride = pitch;
    screen_size = {phys_w, phys_h};

    virt_h *= 2;

    mem_page = palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW | MEM_DEV, false);
    uint32_t fb_bus, fb_size;
    if (!mbox_alloc_fb(virt_w, virt_h, &fb_bus, &fb_size)){
        kprintf("[VIDEOCORE] Failed updating mailbox");
        return false;
    }
    framebuffer = (uint32_t*)(uintptr_t)BUS_ADDRESS(fb_bus);
    framebuffer_size = fb_size/2;
    if (fb_size < virt_h * virt_w * bpp){
        kprintf("[VIDEOCORE] Fallback to one framebuffer with copying. Expected %i, got %i",virt_h * virt_w * bpp,fb_size);
        mailbox_fallback = true;
        back_framebuffer = (uint32_t*)palloc(virt_h * virt_w * bpp, MEM_PRIV_KERNEL, MEM_DEV | MEM_RW, true);
    } else back_framebuffer = (uint32_t*)((uintptr_t)framebuffer + framebuffer_size);

    kprintf("[VIDEOCORE] Size %ix%i (%ix%i) (%ix%i) | %i (%i)",phys_w,phys_h,virt_w,virt_h,screen_size.width,screen_size.height,depth, stride);
    kprintf("[VIDEOCORE] Framebuffer allocated to %x (%i). BPP %i. Stride %i. Backbuffer at %x",framebuffer, framebuffer_size, bpp, stride/bpp,back_framebuffer);
    mark_used((uintptr_t)framebuffer,count_pages(fb_size,PAGE_SIZE));
    for (size_t i = (uintptr_t)framebuffer; i < (uintptr_t)framebuffer + fb_size; i += GRANULE_4KB){
        register_device_memory(i,i);
    }

    ctx = {
        .dirty_rects = {},
        .fb = (uint32_t*)back_framebuffer,
        .stride = stride,
        .width = screen_size.width,
        .height = screen_size.height,
        .dirty_count = 0,
        .full_redraw = 0,
    };

    return true;
}

void VideoCoreGPUDriver::update_gpu_fb(){
    if (screen_size.height == 0 || mailbox_fallback) return;
    last_offset = screen_size.height - last_offset;
    if (!mbox_set_offset(last_offset)){
        kprintf("[VIDEOCORE] failed to swap buffer");
        mailbox_fallback = true;
    }
}

gpu_size VideoCoreGPUDriver::get_screen_size(){
    return screen_size;
}