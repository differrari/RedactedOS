#include "virtio_gpu_pci.hpp"
#include "pci.h"
#include "memory/kalloc.h"
#include "console/kio.h"
#include "graph/font8x8_bridge.h"
#include "ui/draw/draw.h"
#include "std/std.hpp"

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106

#define GPU_RESOURCE_ID 1

#define BPP 4

VirtioGPUDriver* VirtioGPUDriver::try_init(gpu_size preferred_screen_size){
    VirtioGPUDriver* driver = new VirtioGPUDriver();
    if (driver->init(preferred_screen_size))
        return driver;
    delete driver;
    return nullptr;
}

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

    fb_set_stride(screen_size.width * BPP);

    kprintf("Stride %i",screen_size.width * BPP);
    
    framebuffer_size = screen_size.width * screen_size.height * BPP;
    framebuffer_size = (framebuffer_size);
    framebuffer = talloc(framebuffer_size);

    fb_set_bounds(screen_size.width,screen_size.height);
    
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
    //TODO: replace talloc as we did in disk
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
}__attribute__((packed)) virtio_backing_cmd;

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
}__attribute__((packed)) virtio_scanout_cmd;

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
}__attribute__((packed)) virtio_transfer_cmd;

bool VirtioGPUDriver::transfer_to_host(gpu_rect rect) {
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
    cmd->rect.x = rect.point.x;
    cmd->rect.y = rect.point.y;
    cmd->rect.width = rect.size.width;
    cmd->rect.height = rect.size.height;

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
}__attribute__((packed)) virtio_flush_cmd;

void VirtioGPUDriver::flush() {

    if (full_redraw) {
        transfer_to_host((gpu_rect){0,0,screen_size.width,screen_size.height});
    } else {
        for (uint32_t i = 0; i < dirty_count; i++) {
            gpu_rect r = dirty_rects[i];
            transfer_to_host(r);
        }
    }
    
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

int VirtioGPUDriver::try_merge(gpu_rect* a, gpu_rect* b) {
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

void VirtioGPUDriver::mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (x >= screen_size.width || y >= screen_size.height)
        return;

    if (x + w > screen_size.width)
        w = screen_size.width - x;

    if (y + h > screen_size.height)
        h = screen_size.height - y;

    if (w == 0 || h == 0)
        return;

    gpu_rect new_rect = { x, y, w, h };

    for (uint32_t i = 0; i < dirty_count; i++) {
        if (try_merge(&dirty_rects[i], &new_rect))
            return;
    }

    if (dirty_count < MAX_DIRTY_RECTS_VGP)
        dirty_rects[dirty_count++] = new_rect;
    else
        full_redraw = true;
}


void VirtioGPUDriver::clear(uint32_t color) {
    fb_clear((uint32_t*)framebuffer, screen_size.width, screen_size.height, color);
    mark_dirty(0,0,screen_size.width,screen_size.height);
}

void VirtioGPUDriver::draw_pixel(uint32_t x, uint32_t y, color color){
    fb_draw_pixel((uint32_t*)framebuffer, x, y, color);
    mark_dirty(x,y,1,1);
}

void VirtioGPUDriver::fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, color color){
    fb_fill_rect((uint32_t*)framebuffer, x, y, width, height, color);
    mark_dirty(x,y,width,height);
}

void VirtioGPUDriver::draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, color color){
    gpu_rect rect = fb_draw_line((uint32_t*)framebuffer, x0, y0, x1, y1, color);
    mark_dirty(rect.point.x,rect.point.y,rect.size.width,rect.size.height);
}

void VirtioGPUDriver::draw_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color){
    fb_draw_char((uint32_t*)framebuffer, x, y, c, scale, color);
    mark_dirty(x,y,8*scale,8*scale);
}

void VirtioGPUDriver::draw_string(string s, uint32_t x, uint32_t y, uint32_t scale, uint32_t color){
    gpu_size drawn_string = fb_draw_string((uint32_t*)framebuffer, s, x, y, scale, color);
    mark_dirty(x,y,drawn_string.width,drawn_string.height);
}

uint32_t VirtioGPUDriver::get_char_size(uint32_t scale){
    return fb_get_char_size(scale);
}

gpu_size VirtioGPUDriver::get_screen_size(){
    return screen_size;
}
