#include "videocore.hpp"
#include "fw/fw_cfg.h"
#include "memory/talloc.h"
#include "console/kio.h"
#include "ui/draw/draw.h"
#include "std/memory_access.h"
#include "std/std.h"
#include "std/memory.h"
#include "mailbox/mailbox.h"
#include "mailbox/vc_mbox.h"
#include "math/math.h"
#include "memory/mmu.h"
#include "memory/page_allocator.h"

#define RGB_FORMAT_XRGB8888 ((uint32_t)('X') | ((uint32_t)('R') << 8) | ((uint32_t)('2') << 16) | ((uint32_t)('4') << 24))
#define BUS_ADDRESS(addr)   ((addr) & ~0xC0000000)
#define VC_BUFFERS 3

VideoCoreGPUDriver* VideoCoreGPUDriver::try_init(gpu_size preferred_screen_size){
    VideoCoreGPUDriver* driver = new VideoCoreGPUDriver();
    if (driver->init(preferred_screen_size))
        return driver;
    delete driver;
    return nullptr;
}

bool VideoCoreGPUDriver::init(gpu_size preferred_screen_size){
    uint32_t phys_w = 0;
    uint32_t phys_h = 0;
    uint32_t virt_w = 0;
    uint32_t virt_h = 0;
    uint32_t depth  = 0;

    if (!mbox_query_modes(&phys_w, &phys_h, &virt_w, &virt_h, &depth, &stride, RGB_FORMAT_XRGB8888)) return false;
    if (depth != bpp*8) return false;
    if (stride == 0) return false;
    if ((stride % bpp) != 0) return false;

    uint32_t target_w = phys_w ? phys_w : preferred_screen_size.width;
    uint32_t target_h = phys_h ? phys_h : preferred_screen_size.height;
    if (target_w == 0 || target_h == 0) return false;

    uint32_t fb_addr = 0;
    uint32_t fb_size = 0;
    if (!mbox_alloc_fb(target_w, target_h * VC_BUFFERS, &fb_addr, &fb_size)) return false;
    if (fb_addr == 0 || fb_size == 0) return false;

    framebuffer = (uint32_t*)(uintptr_t)BUS_ADDRESS(fb_addr);
    screen_size = (gpu_size){target_w, target_h};

    ctx = (draw_ctx){
        .dirty_rects = {},
        .fb = nullptr,
        .stride = stride,
        .width = screen_size.width,
        .height = screen_size.height,
        .dirty_count = 0,
        .full_redraw = 0,
    };

    uint64_t expected_total = (uint64_t)stride * (uint64_t)screen_size.height * (uint64_t)VC_BUFFERS;
    if ((uint64_t)fb_size < expected_total) return false;

    framebuffer_size = stride * screen_size.height;
    mem_page = palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW | MEM_DEV, false);

    mark_used((uintptr_t)framebuffer, count_pages(fb_size, PAGE_SIZE));
    for (size_t i = (uintptr_t)framebuffer; i < (uintptr_t)framebuffer + fb_size; i += GRANULE_4KB) register_device_memory(i, i);

    last_offset = 0;
    back_framebuffer = (uint32_t*)((uintptr_t)framebuffer + framebuffer_size);
    ctx.fb = (uint32_t*)back_framebuffer;

    return true;
}

void VideoCoreGPUDriver::update_gpu_fb(){
    last_offset = (last_offset + screen_size.height) % (screen_size.height * VC_BUFFERS);
    uint32_t yoff = last_offset;
    if (!mbox_set_offset(yoff)) kprintf("[VIDEOCORE] swap failed");

    uint32_t cur_idx = last_offset / screen_size.height;
    uint32_t draw_idx = (cur_idx + 1) % VC_BUFFERS;

    back_framebuffer = (uint32_t*)((uintptr_t)framebuffer + (uintptr_t)framebuffer_size * draw_idx);
    ctx.fb = (uint32_t*)back_framebuffer;
}

gpu_size VideoCoreGPUDriver::get_screen_size(){
    return screen_size;
}