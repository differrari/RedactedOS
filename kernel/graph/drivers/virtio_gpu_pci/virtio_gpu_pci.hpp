#pragma once 

#include "kstring.h"
#include "virtio/virtio_pci.h"
#include "../gpu_driver.hpp"

class VirtioGPUDriver : GPUDriver {
public:
    bool init(gpu_size preferred_screen_size);

    void flush();

    void clear(color color);
    void draw_pixel(uint32_t x, uint32_t y, color color);
    void draw_single_pixel(uint32_t x, uint32_t y, color color);
    void fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, color color);
    void draw_line(uint32_t x0, uint32_t y0, uint32_t x1,uint32_t y1, color color);
    void draw_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color);
    void draw_single_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color);
    gpu_size get_screen_size();
    void draw_string(kstring s, uint32_t x, uint32_t y, uint32_t scale, uint32_t color);
    uint32_t get_char_size(uint32_t scale);
    
private: 
    gpu_size screen_size;
    virtio_device gpu_dev;
    uintptr_t framebuffer;
    uint64_t framebuffer_size;

    gpu_size get_display_info();
    bool create_2d_resource(gpu_size size);
    bool attach_backing();
    bool set_scanout();
    bool transfer_to_host();

    bool scanout_found;
    uint64_t scanout_id;
};