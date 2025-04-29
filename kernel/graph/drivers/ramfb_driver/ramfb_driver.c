#include "console/kio.h"
#include "ram_e.h"
#include "kstring.h"
#include "fw/fw_cfg.h"
#include "ramfb_driver.h"
#include "graph/font8x8_basic.h"

#define RGB_FORMAT_XRGB8888 ((uint32_t)('X') | ((uint32_t)('R') << 8) | ((uint32_t)('2') << 16) | ((uint32_t)('4') << 24))

typedef struct {
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
}__attribute__((packed)) fb_structure;

uint64_t fb_ptr;

uint32_t width;
uint32_t height;
uint32_t bpp;
uint32_t stride;

void rfb_clear(uint32_t color){
    volatile uint32_t* fb = (volatile uint32_t*)fb_ptr;
    uint32_t pixels = width * height;
    for (uint32_t i = 0; i < pixels; i++) {
        fb[i] = color;
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

void rfb_draw_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color) {
    const uint8_t* glyph = font8x8_basic[(uint8_t)c];
    for (uint32_t row = 0; row < (8 * scale); row++) {
        uint8_t bits = glyph[row/scale];
        for (uint32_t col = 0; col < (8 * scale); col++) {
            if (bits & (1 << (7 - col))) {
                rfb_draw_pixel(x + col, y + row, color);
            }
        }
    }
}

bool rfb_init(uint32_t w, uint32_t h) {

    width = w;
    height = h;

    bpp = 4;
    stride = bpp * width;
    
    struct fw_cfg_file file;
    fw_find_file(string_l("etc/ramfb"), &file);
    
    if (file.selector == 0x0){
        kprintf("Ramfb not found");
        return false;
    }

    fb_ptr = palloc(width * height * bpp);

    fb_structure fb = {
        .addr = __builtin_bswap64(fb_ptr),
        .width = __builtin_bswap32(width),
        .height = __builtin_bswap32(height),
        .fourcc = __builtin_bswap32(RGB_FORMAT_XRGB8888),
        .flags = __builtin_bswap32(0),
        .stride = __builtin_bswap32(stride),
    };

    fw_cfg_dma_write(&fb, sizeof(fb), file.selector);
    
    kprintf("ramfb configured");

    return true;
}

uint32_t rfb_get_char_size(uint32_t scale){
    return 8 * scale;
}

void rfb_draw_string(kstring s, uint32_t x0, uint32_t y0, uint32_t scale){
    int char_size = rfb_get_char_size(scale);
    int str_length = s.length;

    rfb_fill_rect(x0,y0, char_size*str_length,char_size,0x0);
    for (int i = 0; i < str_length; i++){    
        rfb_draw_char(x0 + (i * char_size),y0,s.data[i],scale, 0xFFFFFF);
    }
}


void rfb_flush(){
    
}