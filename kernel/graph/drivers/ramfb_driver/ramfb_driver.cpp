#include "ramfb_driver.h"
#include "ramfb.hpp"


RamFBGPUDriver rfb_driver;

void rfb_clear(uint32_t color){
    rfb_driver.clear(color);
}

void rfb_draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    rfb_driver.draw_pixel(x,y,color);
}

void rfb_draw_single_pixel(uint32_t x, uint32_t y, uint32_t color) {
    rfb_driver.draw_single_pixel(x,y,color);
}

void rfb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    rfb_driver.fill_rect(x,y,w,h,color);
}

void rfb_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color) {
    rfb_driver.draw_line(x0,y0,x1,y1,color);
}

void rfb_draw_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color) {
    rfb_driver.draw_char(x,y,c,scale,color);
}

void rfb_draw_single_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color) {
    rfb_driver.draw_single_char(x,y,c,scale,color);
}

uint32_t rfb_get_char_size(uint32_t scale){
    return rfb_driver.get_char_size(scale);
}

void rfb_draw_string(kstring s, uint32_t x0, uint32_t y0, uint32_t scale, uint32_t color){
    rfb_driver.draw_string(s,x0,y0,scale,color);
}

void rfb_flush() {
    rfb_driver.flush();
}

bool rfb_init(uint32_t w, uint32_t h) {
    return rfb_driver.init((gpu_size){w,h});
}