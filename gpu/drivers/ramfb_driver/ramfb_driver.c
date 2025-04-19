#include "uart.h"
#include "mmio.h"
#include "pci.h"
#include "string.h"

#define FW_CFG_DATA  0x09020000
#define FW_CFG_CTL   (FW_CFG_DATA + 0x8)
#define FW_CFG_DMA   (FW_CFG_DATA + 0x10)

#define FW_LIST_DIRECTORY 0x19

#define FORMAT_SIZE 32

#define FW_CFG_DMA_READ 0x2
#define FW_CFG_DMA_SELECT 0x8
#define FW_CFG_DMA_WRITE 0x10
#define FW_CFG_DMA_ERROR 0x1

#define RGB_FORMAT_XRGB8888 ((uint32_t)('X') | ((uint32_t)('R') << 8) | ((uint32_t)('2') << 16) | ((uint32_t)('4') << 24))

typedef struct {
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
}__attribute__((packed)) fb_structure;

struct fw_cfg_file {
    uint32_t size;
    uint16_t selector;
    uint16_t reserved;
    char name[56];
}__attribute__((packed));

struct fw_cfg_dma_access {
    uint32_t control;
    uint32_t length;
    uint64_t address;
}__attribute__((packed));

uint64_t fb_ptr;

uint32_t width;
uint32_t height;
uint32_t bpp;
uint32_t stride;

bool fw_cfg_check(){
    return read64(FW_CFG_DATA) == 0x554D4551;
}

uint32_t fix_rgb(uint32_t color) {
    return (color & 0xFF0000) >> 16 
         | (color & 0x00FF00)       
         | (color & 0x0000FF) << 16;
}

void rfb_clear(uint32_t color){
    volatile uint32_t* fb = (volatile uint32_t*)fb_ptr;
    uint32_t pixels = width * height;
    for (uint32_t i = 0; i < pixels; i++) {
        fb[i] = fix_rgb(color);
    }
}

void rfb_draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= width || y >= height) return;
    volatile uint32_t* fb = (volatile uint32_t*)fb_ptr;
    fb[y * (stride / 4) + x] = color;
}

void rfb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t dy = 0; dy < h; dy++) {
        for (uint32_t dx = 0; dx < w; dx++) {
            rfb_draw_pixel(x + dx, y + dy, color);
        }
    }
}

void rfb_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sy = (y0 < y1) ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2, e2;

    for (;;) {
        rfb_draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 < dy) { err += dx; y0 += sy; }
    }
}

void fw_cfg_dma_read(void* dest, uint32_t size, uint32_t ctrl) {
    struct fw_cfg_dma_access access = {
        .address = __builtin_bswap64((uint64_t)dest),
        .length = __builtin_bswap32(size),
        .control = __builtin_bswap32(ctrl),
    };

    write64(FW_CFG_DMA, __builtin_bswap64((uint64_t)&access));

    __asm__("ISB");

    while (__builtin_bswap32(access.control) & ~0x1) {}
    
}

struct fw_cfg_file* fw_find_file(string search) {

    struct fw_cfg_file *file = (struct fw_cfg_file *)alloc(sizeof(struct fw_cfg_file));
    uint32_t count;
    fw_cfg_dma_read(&count, sizeof(count), (FW_LIST_DIRECTORY << 16) | FW_CFG_DMA_SELECT | FW_CFG_DMA_READ);

    count = __builtin_bswap32(count);

    uart_puts("There are ");
    uart_puthex(count);
    uart_puts(" values in directory\n");

    for (uint32_t i = 0; i < count; i++) {

        fw_cfg_dma_read(file, sizeof(struct fw_cfg_file), FW_CFG_DMA_READ);

        file->size = __builtin_bswap32(file->size);
        file->selector = __builtin_bswap16(file->selector);

        string filename = string_c(file->name, 56);
        if (string_equals(filename, search)){
            uart_puts("Found device at selector ");
            uart_puthex(file->selector);
            uart_putc('\n');
            return file;
        }
    }

    return (struct fw_cfg_file*)0;
}

bool rfb_init(uint32_t w, uint32_t h) {

    if (!fw_cfg_check()){
        uart_puts("Wrong FW_CFG config");
        return false;
    }

    width = w;
    height = h;

    bpp = 4;
    stride = bpp * width;

    fb_ptr = alloc(width * height * bpp);

    fb_structure fb = {
        .addr = __builtin_bswap64(fb_ptr),
        .width = __builtin_bswap32(width),
        .height = __builtin_bswap32(height),
        .fourcc = __builtin_bswap32(RGB_FORMAT_XRGB8888),
        .flags = __builtin_bswap32(0),
        .stride = __builtin_bswap32(stride),
    };

    struct fw_cfg_file *file = fw_find_file(string_l("etc/ramfb"));

    if (file->selector == 0x0){
        uart_puts("Ramfb not found\n");
        return false;
    }
    
    uint32_t control = (file->selector << 16) | FW_CFG_DMA_SELECT | FW_CFG_DMA_WRITE;
    fw_cfg_dma_read(&fb, sizeof(fb), control);
    
    uart_puts("ramfb configured\n");

    free(file);

    return true;
}

void rfb_flush(){
    
}