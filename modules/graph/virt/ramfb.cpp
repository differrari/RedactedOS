#include "ramfb.hpp"
#include "console/kio.h"
#include "ui/draw/draw.h"
#include "std/memory_access.h"
#include "std/std.h"
#include "std/memory.h"
#include "theme/theme.h"
#include "memory/page_allocator.h"
#include "sysregs.h"

typedef struct {
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
}__attribute__((packed)) ramfb_structure;

#define RGB_FORMAT_XRGB8888 ((uint32_t)('X') | ((uint32_t)('R') << 8) | ((uint32_t)('2') << 16) | ((uint32_t)('4') << 24))
#define RGB_FORMAT_ARGB8888 ((uint32_t)('A') | ((uint32_t)('R') << 8) | ((uint32_t)('2') << 16) | ((uint32_t)('4') << 24))

#define bpp 4

RamFBGPUDriver* RamFBGPUDriver::try_init(gpu_size preferred_screen_size){
    RamFBGPUDriver* driver = new RamFBGPUDriver();
    if (driver->init(preferred_screen_size))
        return driver;
    delete driver;
    return nullptr;
}

bool RamFBGPUDriver::init(gpu_size preferred_screen_size){
    file = {};
    screen_size = preferred_screen_size;

    if (!screen_size.width || !screen_size.height) return false;

    stride = bpp * screen_size.width;
    framebuffer_size = (size_t)(stride * screen_size.height);

    fw_find_file("etc/ramfb", &file);
    
    if (file.selector == 0x0){
        kprintf("Ramfb not found");
        return false;
    }
    mem_page = palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW | MEM_DEV, false);
    uint8_t* fb_block = (uint8_t*)palloc(framebuffer_size*2, MEM_PRIV_SHARED, MEM_RW, true);

    if (!fb_block) return false;

    framebuffer = (uint32_t*)fb_block;
    back_framebuffer = (uint32_t*)(fb_block + framebuffer_size);

    ctx = {
        .dirty_rects = {},
        .fb = (uint32_t*)back_framebuffer,
        .stride = stride,
        .width = screen_size.width,
        .height = screen_size.height,
        .dirty_count = 0,
        .full_redraw = 0,
    };

    update_gpu_fb();
    
    kprintf("[RAMFB] configured");

    return true;
}

void RamFBGPUDriver::update_gpu_fb(){
    ramfb_structure fb = {
        .addr = __builtin_bswap64(VIRT_TO_PHYS((uintptr_t)framebuffer)),
        .fourcc = __builtin_bswap32(RGB_FORMAT_ARGB8888),
        .flags = __builtin_bswap32(0),
        .width = __builtin_bswap32(screen_size.width),
        .height = __builtin_bswap32(screen_size.height),
        .stride = __builtin_bswap32(stride),
    };

    fw_cfg_dma_write(&fb, sizeof(fb), file.selector);
}

gpu_size RamFBGPUDriver::get_screen_size(){
    return screen_size;
}