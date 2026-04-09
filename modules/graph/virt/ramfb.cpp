#include "ramfb.hpp"
#include "console/kio.h"
#include "ui/draw/draw.h"
#include "std/memory_access.h"
#include "std/std.h"
#include "std/memory.h"
#include "theme/theme.h"
#include "memory/page_allocator.h"
#include "sysregs.h"
#include "memory/addr.h"

bool verbose = true;
#define kprintfv(fmt, ...) \
    ({ \
        if (verbose){\
            kprintf(fmt, ##__VA_ARGS__); \
        }\
    })

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
    kprintfv("[RAMFB] init requested %ix%i", preferred_screen_size.width, preferred_screen_size.height);

    if (!screen_size.width || !screen_size.height) return false;

    stride = bpp * screen_size.width;
    framebuffer_size = (size_t)(stride * screen_size.height);

    kprintfv("[RAMFB] probing cfg desc");
    if (!fw_find_file("etc/ramfb", &file) || file.selector == 0x0){
        kprintf("Ramfb not found");
        return false;
    }

    kprintfv("[RAMFB] descriptor found selector=%x size=%x name=%s", file.selector, file.size, file.name);
    mem_page = palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW | MEM_DEV, false);
    uint8_t* fb_block = (uint8_t*)palloc(framebuffer_size*2, MEM_PRIV_SHARED, MEM_RW, true);

    if (!fb_block) {
        kprintfv("[RAMFB] failed to allocate fb");
        return false;
    }

    framebuffer = (uint32_t*)fb_block;
    back_framebuffer = (uint32_t*)(fb_block + framebuffer_size);

    kprintfv("[RAMFB] framebuffer va=%x pa=%x", (uintptr_t)framebuffer, (uintptr_t)pt_va_to_pa(framebuffer));
    kprintfv("[RAMFB] backbuffer va=%x pa=%x", (uintptr_t)back_framebuffer, (uintptr_t)pt_va_to_pa(back_framebuffer));

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
    paddr_t fb_pa = pt_va_to_pa(framebuffer);
    ramfb_structure fb = {
        .addr = __builtin_bswap64((uint64_t)fb_pa),
        .fourcc = __builtin_bswap32(RGB_FORMAT_ARGB8888),
        .flags = __builtin_bswap32(0),
        .width = __builtin_bswap32(screen_size.width),
        .height = __builtin_bswap32(screen_size.height),
        .stride = __builtin_bswap32(stride),
    };

    kprintfv("[RAMFB] writing descriptor selector=%x fb_pa=%x %ix%i stride=%x", file.selector, (uintptr_t)fb_pa, screen_size.width, screen_size.height, stride);
    fw_cfg_dma_write(&fb, sizeof(fb), file.selector);
    kprintfv("[RAMFB] descriptor w completed");
}

gpu_size RamFBGPUDriver::get_screen_size(){
    return screen_size;
}