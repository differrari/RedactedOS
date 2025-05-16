#include "console/kio.h"
#include "ram_e.h"
#include "kstring.h"
#include "fw/fw_cfg.h"
#include "ramfb_driver.h"
#include "graph/font8x8_basic.h"
#include "graphic_types.h"

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
uint64_t bfb_ptr;

uint32_t width;
uint32_t height;
uint32_t bpp;
uint32_t stride;

#define MAX_DIRTY_RECTS 64
gpu_rect dirty_rects[MAX_DIRTY_RECTS];
uint32_t dirty_count = 0;
bool full_redraw = false;

int try_merge(gpu_rect* a, gpu_rect* b) {
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

void mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    gpu_rect new_rect = { x, y, w, h };

    for (int i = 0; i < dirty_count; i++) {
        if (try_merge(&dirty_rects[i], &new_rect))
            return;
    }

    if (dirty_count < MAX_DIRTY_RECTS)
        dirty_rects[dirty_count++] = new_rect;
    else
        full_redraw = true; // optional: fallback to full redraw
}

void rfb_clear(uint32_t color){
    volatile uint32_t* fb = (volatile uint32_t*)bfb_ptr;
    uint32_t pixels = width * height;
    for (uint32_t i = 0; i < pixels; i++) {
        fb[i] = color;
    }
    mark_dirty(0,0,width,height);
}

void rfb_draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= width || y >= height) return;
    volatile uint32_t* fb = (volatile uint32_t*)bfb_ptr;
    fb[y * (stride / 4) + x] = color;
}

void rfb_draw_single_pixel(uint32_t x, uint32_t y, uint32_t color) {
    rfb_draw_pixel(x,y, color);
    mark_dirty(x,y,1,1);
}

void rfb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t dy = 0; dy < h; dy++) {
        for (uint32_t dx = 0; dx < w; dx++) {
            rfb_draw_pixel(x + dx, y + dy, color);
        }
    }
    mark_dirty(x,y,w,h);
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

    int min_x = (x0 < x1) ? x0 : x1;
    int min_y = (y0 < y1) ? y0 : y1;
    int max_x = (x0 > x1) ? x0 : x1;
    int max_y = (y0 > y1) ? y0 : y1;

    mark_dirty(min_x, min_y, max_x - min_x + 1, max_y - min_y + 1);
}

void rfb_draw_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color) {
    const uint8_t* glyph = font8x8_basic[(uint8_t)c];
    for (uint32_t row = 0; row < (8 * scale); row++) {
        uint8_t bits = glyph[row/scale];
        for (uint32_t col = 0; col < (8 * scale); col++) {
            if (bits & (1 << (7 - (col / scale)))) {
                rfb_draw_pixel(x + col, y + row, color);
            }
        }
    }
}

void rfb_draw_single_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color) {
    rfb_draw_char(x,y,c,scale,color);
    mark_dirty(x,y,8*scale,8*scale);
}

uint32_t rfb_get_char_size(uint32_t scale){
    return 8 * scale;
}

#define line_height char_size + 2

void rfb_draw_string(kstring s, uint32_t x0, uint32_t y0, uint32_t scale, uint32_t color){
    int char_size = rfb_get_char_size(scale);
    int str_length = s.length;
    
    uint32_t xoff = 0;
    uint32_t xSize = 0;
    uint32_t xRowSize = 0;
    uint32_t ySize = line_height;
    for (int i = 0; i < str_length; i++){    
        char c = s.data[i];
        if (c == '\n'){
            y0 += line_height; 
            ySize += line_height;
            if (xRowSize > xSize)
                xSize = xRowSize;
            xoff = 0;
        } else {
            rfb_draw_char(x0 + (xoff * char_size),y0,c,scale, color);
            xoff++;
            xRowSize += char_size;
        }
    }
    if (xRowSize > xSize)
        xSize = xRowSize;
    mark_dirty(x0,y0,xSize,ySize);
}

void rfb_flush() {
    if (full_redraw) {
        memcpy((void*)fb_ptr, (void*)bfb_ptr, width * height * bpp);
        dirty_count = 0;
        full_redraw = false;
        return;
    }
    
    volatile uint32_t* fb = (volatile uint32_t*)fb_ptr;
    volatile uint32_t* bfb = (volatile uint32_t*)bfb_ptr;
    
    for (int i = 0; i < dirty_count; i++) {
        gpu_rect r = dirty_rects[i];
        
        for (uint32_t y = 0; y < r.size.height; y++) {
            uint32_t dest_y = r.point.y + y;
            if (dest_y >= height) break;
            
            uint32_t* dst = (uint32_t*)&fb[dest_y * (stride / 4) + r.point.x];
            uint32_t* src = (uint32_t*)&bfb[dest_y * (stride / 4) + r.point.x];
            
            uint32_t copy_width = r.size.width;
            if (r.point.x + copy_width > width)
            copy_width = width - r.point.x;
            
            memcpy(dst, src, copy_width * sizeof(uint32_t));
        }
    }
    
    full_redraw = false;
    dirty_count = 0;
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
    bfb_ptr = palloc(width * height * bpp);

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