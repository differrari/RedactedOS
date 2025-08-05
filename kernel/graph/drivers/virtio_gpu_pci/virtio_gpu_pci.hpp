#pragma once 

#include "virtio/virtio_pci.h"
#include "../gpu_driver.hpp"

#define VIRTIO_GPU_ID 0x1050

class VirtioGPUDriver : public GPUDriver {
public:
    static VirtioGPUDriver* try_init(gpu_size preferred_screen_size);
    VirtioGPUDriver(){}
    bool init(gpu_size preferred_screen_size) override;

    void flush() override;

    void clear(color color) override;
    void draw_pixel(uint32_t x, uint32_t y, color color) override;
    void fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, color color) override;
    void draw_line(uint32_t x0, uint32_t y0, uint32_t x1,uint32_t y1, color color) override;
    void draw_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color) override;
    gpu_size get_screen_size() override;
    void draw_string(string s, uint32_t x, uint32_t y, uint32_t scale, uint32_t color) override;
    uint32_t get_char_size(uint32_t scale) override;
    ~VirtioGPUDriver() = default;
    
private: 
    gpu_size screen_size;
    virtio_device gpu_dev;
    uintptr_t framebuffer;
    uint64_t framebuffer_size;

    gpu_size get_display_info();
    bool create_2d_resource(gpu_size size);
    bool attach_backing();
    bool set_scanout();
    bool transfer_to_host(gpu_rect rect);

    bool scanout_found;
    uint64_t scanout_id;
};