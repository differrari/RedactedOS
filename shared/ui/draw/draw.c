#include "draw.h"
#include "graph/font8x8_bridge.h"
#include "kstring.h"

#define line_height char_size + 2

uint32_t stride = 0;
uint32_t max_width, max_height;

void fb_clear(uint32_t* fb, uint32_t width, uint32_t height, uint32_t color) {
    for (uint32_t i = 0; i < width * height; i++) {
        fb[i] = color;
    }
}

void fb_draw_pixel(uint32_t* fb, uint32_t x, uint32_t y, color color){
    if (x > max_width || y > max_height) return;
    fb[y * (stride / 4) + x] = color;
}

void fb_fill_rect(uint32_t* fb, uint32_t x, uint32_t y, uint32_t width, uint32_t height, color color){
    for (uint32_t dy = 0; dy < height; dy++) {
        for (uint32_t dx = 0; dx < width; dx++) {
            fb_draw_pixel(fb, x + dx, y + dy, color);
        }
    }
}

gpu_rect fb_draw_line(uint32_t* fb, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, color color){
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sy = (y0 < y1) ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2, e2;

    for (;;) {
        fb_draw_pixel(fb, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 < dy) { err += dx; y0 += sy; }
    }

    int min_x = (x0 < x1) ? x0 : x1;
    int min_y = (y0 < y1) ? y0 : y1;
    int max_x = (x0 > x1) ? x0 : x1;
    int max_y = (y0 > y1) ? y0 : y1;

    return (gpu_rect) { {min_x, min_y}, {max_x - min_x + 1, max_y - min_y + 1}};
}

void fb_draw_char(uint32_t* fb, uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color){
    const uint8_t* glyph = get_font8x8((uint8_t)c);
    for (uint32_t row = 0; row < (8 * scale); row++) {
        uint8_t bits = glyph[row/scale];
        for (uint32_t col = 0; col < (8 * scale); col++) {
            if (bits & (1 << (7 - (col / scale)))) {
                fb_draw_pixel(fb, x + col, y + row, color);
            }
        }
    }
}

gpu_size fb_draw_string(uint32_t* fb, string s, uint32_t x0, uint32_t y0, uint32_t scale, uint32_t color){
    int char_size = fb_get_char_size(scale);
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
            xRowSize = 0;
            xoff = 0;
        } else {
            fb_draw_char(fb, x0 + (xoff * char_size),y0,c,scale, color);
            xoff++;
            xRowSize += char_size;
        }
    }
    if (xRowSize > xSize)
        xSize = xRowSize;

    return (gpu_size){xSize,ySize};
}

uint32_t fb_get_char_size(uint32_t scale){
    return 8 * scale;
}

void fb_set_stride(uint32_t new_stride){
    stride = new_stride;
}

void fb_set_bounds(uint32_t width, uint32_t height){
    max_width = width;
    max_height = height;
}