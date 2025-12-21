#pragma once 

#include "virtio/virtio_pci.h"
#include "common/gpu_driver.hpp"

#define VIRTIO_GPU_ID 0x1050

typedef struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
} virtio_gpu_ctrl_hdr;

struct virtio_gpu_cursor_pos { 
    uint32_t scanout_id; 
    uint32_t x; 
    uint32_t y; 
    uint32_t padding;
}; 
 
struct virtio_gpu_update_cursor { 
    virtio_gpu_ctrl_hdr hdr; 
    virtio_gpu_cursor_pos pos; 
    uint32_t resource_id; 
    uint32_t hot_x; 
    uint32_t hot_y; 
    uint32_t padding;
};
static_assert(sizeof(virtio_gpu_update_cursor) == 56, "Update cursor command must be 56 bytes");

typedef struct virtio_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} virtio_rect;

typedef struct virtio_flush_cmd {
    struct virtio_gpu_ctrl_hdr hdr; 
    struct virtio_rect rect; 
    uint32_t resource_id; 
    uint32_t padding; 
}__attribute__((packed)) virtio_flush_cmd;

typedef struct virtio_transfer_cmd {
    struct virtio_gpu_ctrl_hdr hdr; 
    struct virtio_rect rect; 
    uint64_t offset; 
    uint32_t resource_id; 
    uint32_t padding; 
}__attribute__((packed)) virtio_transfer_cmd;

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

    draw_ctx* get_ctx() override;

    void setup_cursor() override;    
    void update_cursor(uint32_t x, uint32_t y, bool full) override;
    void set_cursor_pressed(bool pressed) override;

    void create_window(uint32_t x, uint32_t y, uint32_t width, uint32_t height, draw_ctx *ctx) override;
    void resize_window(uint32_t width, uint32_t height, draw_ctx *win_ctx) override;

    ~VirtioGPUDriver() = default;
    
private: 
    gpu_size screen_size;
    virtio_device gpu_dev;
    uintptr_t framebuffer;
    uint64_t framebuffer_size;

    gpu_size get_display_info();
    bool create_2d_resource(uint32_t resource_id, gpu_size size);
    bool attach_backing(uint32_t resource_id, sizedptr ptr);
    bool set_scanout();
    bool transfer_to_host(uint32_t resource_id, gpu_rect rect);
    void get_capset(uint32_t capset);
    uint32_t new_resource_id();
    uint32_t new_cursor(uint32_t color);

    uint32_t resource_id_counter;

    uint32_t fb_resource_id;
    uint32_t cursor_resource_id;
    uint32_t cursor_pressed_resource_id;
    uint32_t cursor_unpressed_resource_id;

    virtio_gpu_ctrl_hdr *trans_resp, *flush_resp, *cursor_resp;
    virtio_gpu_update_cursor *cursor_cmd;
    virtio_transfer_cmd *trans_cmd;
    virtio_flush_cmd *flush_cmd;

    draw_ctx ctx;

    bool scanout_found;
    uint64_t scanout_id;
};