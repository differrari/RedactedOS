#include "virtio_gpu_pci_driver.h"
#include "virtio_gpu_pci.hpp"

VirtioGPUDriver driver;

bool vgp_init(uint32_t width, uint32_t height) {
    return driver.init((gpu_size){width,height});
}

void vgp_clear(uint32_t color) {
    driver.clear(color);
}

void vgp_draw_pixel(uint32_t x, uint32_t y, uint32_t color){
    driver.draw_pixel(x,y,color);
}

void vgp_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color){
    driver.fill_rect(x,y,w,h,color);
}

void vgp_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color){
    driver.draw_line(x0,y0,x1,y1,color);
}

void vgp_draw_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color){
    driver.draw_char(x,y,c,scale, color);
}

void vgp_draw_string(kstring s, uint32_t x, uint32_t y, uint32_t scale, uint32_t color){
    driver.draw_string(s, x, y, scale, color);
}

uint32_t vgp_get_char_size(uint32_t scale){
    return driver.get_char_size(scale);
}

gpu_size vgp_get_screen_size(){
    return driver.get_screen_size();
}

void vgp_flush() {
    driver.flush();
}