#include "ramfb.hpp"
#include "console/kio.h"
#include "ui/draw/draw.h"
#include "std/memory_access.h"
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
    file = {};
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
    
    kprintf("[RAMFB] configured");

    return true;
}

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

gpu_size RamFBGPUDriver::get_screen_size(){
    return screen_size;
}