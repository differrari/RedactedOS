#include "virtio_gpu_pci.hpp"
#include "pci.h"
#include "memory/kalloc.h"
#include "console/kio.h"
#include "graph/font8x8_bridge.h"

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106

#define GPU_RESOURCE_ID 1

#define BPP 4

bool VirtioGPUDriver::init(gpu_size preferred_screen_size){
    
    uint64_t addr = find_pci_device(0x1AF4, 0x1050);
    if (!addr){ 
        kprintf("Virtio GPU not found");
        return false;
    }

    pci_enable_device(addr);

    uint64_t device_address, device_size;

    virtio_get_capabilities(&gpu_dev, addr, &device_address, &device_size);
    pci_register(device_address, device_size);
    if (!virtio_init_device(&gpu_dev)) {
        kprintf("Failed initialization");
        return false;
    }

    kprintf("GPU initialized. Issuing commands");

    screen_size = get_display_info();

    kprintf("Returned display size %ix%i", screen_size.width,screen_size.height);
    if (screen_size.width == 0 || screen_size.height == 0){
        return false;
    }

    stride = screen_size.width * BPP;
    
    framebuffer_size = screen_size.width * screen_size.height * BPP;
    framebuffer_size = (framebuffer_size);
    framebuffer = talloc(framebuffer_size);
    
    if (!create_2d_resource(screen_size)) return false;
    
    if (!attach_backing()) return false;

    if (scanout_found)
        set_scanout();
    else
        kprintf("GPU did not return valid scanout data");

    kprintf("GPU ready");

    return true;
}

#define VIRTIO_GPU_MAX_SCANOUTS 16 

typedef struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
} virtio_gpu_ctrl_hdr;

typedef struct virtio_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} virtio_rect;

typedef struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one {
        virtio_rect rect;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} virtio_gpu_resp_display_info;

typedef struct virtio_2d_resource {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} virtio_2d_resource;

gpu_size VirtioGPUDriver::get_display_info(){
    virtio_gpu_ctrl_hdr* cmd = (virtio_gpu_ctrl_hdr*)talloc(sizeof(virtio_gpu_ctrl_hdr));
    cmd->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    cmd->flags = 0;
    cmd->fence_id = 0;
    cmd->ctx_id = 0;
    cmd->ring_idx = 0;
    cmd->padding[0] = 0;
    cmd->padding[1] = 0;
    cmd->padding[2] = 0;

    virtio_gpu_resp_display_info* resp = (virtio_gpu_resp_display_info*)talloc(sizeof(virtio_gpu_resp_display_info));

    if (!virtio_send(&gpu_dev, gpu_dev.common_cfg->queue_desc, gpu_dev.common_cfg->queue_driver, gpu_dev.common_cfg->queue_device,
        (uintptr_t)cmd, sizeof(virtio_gpu_ctrl_hdr), (uintptr_t)resp, sizeof(virtio_gpu_resp_display_info), VIRTQ_DESC_F_WRITE)){
        temp_free(cmd, sizeof(virtio_gpu_ctrl_hdr));
        temp_free(resp, sizeof(virtio_gpu_resp_display_info));
        return (gpu_size){0, 0};
    }

    if (resp->hdr.type != 0x1101) {
        temp_free(cmd, sizeof(virtio_gpu_ctrl_hdr));
        temp_free(resp, sizeof(virtio_gpu_resp_display_info));
        return (gpu_size){0, 0};
    }

    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++){
        if (resp->pmodes[i].enabled) {
            scanout_id = i;
            scanout_found = true;
            gpu_size size = {resp->pmodes[i].rect.width, resp->pmodes[i].rect.height};
            temp_free(cmd, sizeof(virtio_gpu_ctrl_hdr));
            temp_free(resp, sizeof(virtio_gpu_resp_display_info));
            return size;
        }
    }

    scanout_found = false;
    temp_free(cmd, sizeof(virtio_gpu_ctrl_hdr));
    temp_free(resp, sizeof(virtio_gpu_resp_display_info));
    return (gpu_size){0, 0};
}

bool VirtioGPUDriver::create_2d_resource(gpu_size size) {
    virtio_2d_resource* cmd = (virtio_2d_resource*)talloc(sizeof(virtio_2d_resource));
    
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = 0;
    cmd->hdr.ctx_id = 0;
    cmd->hdr.ring_idx = 0;
    cmd->hdr.padding[0] = 0;
    cmd->hdr.padding[1] = 0;
    cmd->hdr.padding[2] = 0;
    cmd->resource_id = GPU_RESOURCE_ID;
    cmd->format = 1; // VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM
    cmd->width = size.width;
    cmd->height = size.height;

    virtio_gpu_ctrl_hdr* resp = (virtio_gpu_ctrl_hdr*)talloc(sizeof(virtio_gpu_ctrl_hdr));

    if (!virtio_send(&gpu_dev, gpu_dev.common_cfg->queue_desc, gpu_dev.common_cfg->queue_driver, gpu_dev.common_cfg->queue_device,
        (uintptr_t)cmd, sizeof(virtio_2d_resource), (uintptr_t)resp, sizeof(virtio_gpu_ctrl_hdr), VIRTQ_DESC_F_WRITE)){
        temp_free(cmd, sizeof(virtio_2d_resource));
        temp_free(resp, sizeof(virtio_gpu_ctrl_hdr));
        return false;
    }
    
    if (resp->type != 0x1100) {
        temp_free(cmd, sizeof(virtio_2d_resource));
        temp_free(resp, sizeof(virtio_gpu_ctrl_hdr));
        return false;
    }

    temp_free(cmd, sizeof(virtio_2d_resource));
    temp_free(resp, sizeof(virtio_gpu_ctrl_hdr));

    return true;
}

typedef struct virtio_backing_cmd {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    struct virtio_backing_entry {
        uint64_t addr;
        uint32_t length;
        uint32_t padding;
    }__attribute__((packed)) entries[1];
}__attribute__((packed));

bool VirtioGPUDriver::attach_backing() {
    virtio_backing_cmd* cmd = (virtio_backing_cmd*)talloc(sizeof(virtio_backing_cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = 0;
    cmd->hdr.ctx_id = 0;
    cmd->hdr.ring_idx = 0;
    cmd->hdr.padding[0] = 0;
    cmd->hdr.padding[1] = 0;
    cmd->hdr.padding[2] = 0;
    cmd->resource_id = GPU_RESOURCE_ID;
    cmd->nr_entries = 1;
    
    cmd->entries[0].addr = framebuffer;
    cmd->entries[0].length = framebuffer_size;
    cmd->entries[0].padding = 0;

    virtio_gpu_ctrl_hdr* resp = (virtio_gpu_ctrl_hdr*)talloc(sizeof(virtio_gpu_ctrl_hdr));

    if (!virtio_send2(&gpu_dev, gpu_dev.common_cfg->queue_desc, gpu_dev.common_cfg->queue_driver, gpu_dev.common_cfg->queue_device,
        (uintptr_t)cmd, sizeof(*cmd), (uintptr_t)resp, sizeof(virtio_gpu_ctrl_hdr), VIRTQ_DESC_F_NEXT)){
        temp_free(cmd, sizeof(*cmd));
        temp_free(resp, sizeof(virtio_gpu_ctrl_hdr));
        return false;
    }

    if (resp->type != 0x1100) {
        temp_free(cmd, sizeof(virtio_backing_cmd));
        temp_free(resp, sizeof(virtio_gpu_ctrl_hdr));
        return false;
    }

    temp_free(cmd, sizeof(virtio_backing_cmd));
    temp_free(resp, sizeof(virtio_gpu_ctrl_hdr));
    return true;
}

typedef struct virtio_scanout_cmd {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
}__attribute__((packed));

bool VirtioGPUDriver::set_scanout() {
    virtio_scanout_cmd* cmd = (virtio_scanout_cmd*)talloc(sizeof(virtio_scanout_cmd));
    
    cmd->r.x = 0;
    cmd->r.y = 0;
    cmd->r.width = screen_size.width;
    cmd->r.height = screen_size.height;

    cmd->scanout_id = scanout_id;
    cmd->resource_id = GPU_RESOURCE_ID;

    cmd->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = 0;
    cmd->hdr.ctx_id = 0;
    cmd->hdr.ring_idx = 0;
    cmd->hdr.padding[0] = 0;
    cmd->hdr.padding[1] = 0;
    cmd->hdr.padding[2] = 0;

    virtio_gpu_ctrl_hdr* resp = (virtio_gpu_ctrl_hdr*)talloc(sizeof(virtio_gpu_ctrl_hdr));

    if (!virtio_send(&gpu_dev, gpu_dev.common_cfg->queue_desc, gpu_dev.common_cfg->queue_driver, gpu_dev.common_cfg->queue_device,
        (uintptr_t)cmd, sizeof(*cmd), (uintptr_t)resp, sizeof(virtio_gpu_ctrl_hdr), VIRTQ_DESC_F_WRITE)){
        temp_free(cmd, sizeof(*cmd));
        temp_free(resp, sizeof(*resp));
        return false;
    }

    if (resp->type != 0x1100) {
        temp_free(cmd, sizeof(*cmd));
        temp_free(resp, sizeof(*resp));
        return false;
    }

    temp_free(cmd, sizeof(*cmd));
    temp_free(resp, sizeof(*resp));
    return true;
}

typedef struct virtio_transfer_cmd {
     struct virtio_gpu_ctrl_hdr hdr; 
    struct virtio_rect rect; 
    uint64_t offset; 
    uint32_t resource_id; 
    uint32_t padding; 
};

bool VirtioGPUDriver::transfer_to_host() {
    virtio_transfer_cmd* cmd = (virtio_transfer_cmd*)talloc(sizeof(virtio_transfer_cmd));
    
    cmd->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = 0;
    cmd->hdr.padding[0] = 0;
    cmd->hdr.padding[1] = 0;
    cmd->hdr.padding[2] = 0;
    cmd->resource_id = GPU_RESOURCE_ID;
    cmd->offset = 0;
    cmd->padding = 0;
    cmd->rect.x = 0;
    cmd->rect.y = 0;
    cmd->rect.width = screen_size.width;
    cmd->rect.height = screen_size.height;

    virtio_gpu_ctrl_hdr* resp = (virtio_gpu_ctrl_hdr*)talloc(sizeof(virtio_gpu_ctrl_hdr));

    if (!virtio_send(&gpu_dev, gpu_dev.common_cfg->queue_desc, gpu_dev.common_cfg->queue_driver, gpu_dev.common_cfg->queue_device,
        (uintptr_t)cmd, sizeof(*cmd), (uintptr_t)resp, sizeof(*resp), VIRTQ_DESC_F_WRITE)){
        temp_free(cmd, sizeof(*cmd));
        temp_free(resp, sizeof(*resp));
        return false;
    }

    if (resp->type != 0x1100) {
        temp_free(cmd, sizeof(*cmd));
        temp_free(resp, sizeof(*resp));
        return false;
    }

    temp_free(cmd, sizeof(*cmd));
    temp_free(resp, sizeof(*resp));
    return true;
}

typedef struct virtio_flush_cmd {
     struct virtio_gpu_ctrl_hdr hdr; 
    struct virtio_rect rect; 
    uint32_t resource_id; 
    uint32_t padding; 
};

void VirtioGPUDriver::flush() {

    transfer_to_host();

    virtio_flush_cmd* cmd = (virtio_flush_cmd*)talloc(sizeof(virtio_flush_cmd));
    
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = 0;
    cmd->hdr.padding[0] = 0;
    cmd->hdr.padding[1] = 0;
    cmd->hdr.padding[2] = 0;
    cmd->resource_id = GPU_RESOURCE_ID;
    cmd->padding = 0;
    cmd->rect.x = 0;
    cmd->rect.y = 0;
    cmd->rect.width = screen_size.width;
    cmd->rect.height = screen_size.height;

    virtio_gpu_ctrl_hdr* resp = (virtio_gpu_ctrl_hdr*)talloc(sizeof(virtio_gpu_ctrl_hdr));

    if (!virtio_send(&gpu_dev, gpu_dev.common_cfg->queue_desc, gpu_dev.common_cfg->queue_driver, gpu_dev.common_cfg->queue_device,
        (uintptr_t)cmd, sizeof(*cmd), (uintptr_t)resp, sizeof(*resp), VIRTQ_DESC_F_WRITE)){
        temp_free(cmd, sizeof(*cmd));
        temp_free(resp, sizeof(*resp));
        return;
    }

    if (resp->type != 0x1100) {
        temp_free(cmd, sizeof(*cmd));
        temp_free(resp, sizeof(*resp));
        return;
    }

    temp_free(cmd, sizeof(*cmd));
    temp_free(resp, sizeof(*resp));
    return;
}

#define line_height char_size + 2

void VirtioGPUDriver::clear(uint32_t color) {

    volatile uint32_t* fb = (volatile uint32_t*)framebuffer;
    for (uint32_t i = 0; i < screen_size.width * screen_size.height; i++) {
        fb[i] = color;
    }
}

void VirtioGPUDriver::draw_pixel(uint32_t x, uint32_t y, color color){
    if (x >= screen_size.width || y >= screen_size.height) return;
    volatile uint32_t* fb = (volatile uint32_t*)framebuffer;
    fb[y * (stride / 4) + x] = color;
}

void VirtioGPUDriver::fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, color color){
    for (uint32_t dy = 0; dy < height; dy++) {
        for (uint32_t dx = 0; dx < width; dx++) {
            draw_pixel(x + dx, y + dy, color);
        }
    }
}

void VirtioGPUDriver::draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, color color){
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sy = (y0 < y1) ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2, e2;

    for (;;) {
        draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 < dy) { err += dx; y0 += sy; }
    }

    int min_x = (x0 < x1) ? x0 : x1;
    int min_y = (y0 < y1) ? y0 : y1;
    int max_x = (x0 > x1) ? x0 : x1;
    int max_y = (y0 > y1) ? y0 : y1;
}

void VirtioGPUDriver::draw_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color){
    const uint8_t* glyph = get_font8x8((uint8_t)c);
    for (uint32_t row = 0; row < (8 * scale); row++) {
        uint8_t bits = glyph[row/scale];
        for (uint32_t col = 0; col < (8 * scale); col++) {
            if (bits & (1 << (7 - (col / scale)))) {
                draw_pixel(x + col, y + row, color);
            }
        }
    }
}

void VirtioGPUDriver::draw_string(kstring s, uint32_t x0, uint32_t y0, uint32_t scale, uint32_t color){
    int char_size = get_char_size(scale);
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
            draw_char(x0 + (xoff * char_size),y0,c,scale, color);
            xoff++;
            xRowSize += char_size;
        }
    }
    if (xRowSize > xSize)
        xSize = xRowSize;
}

uint32_t VirtioGPUDriver::get_char_size(uint32_t scale){
    return 8 * scale;
}
