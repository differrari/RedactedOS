#include "virtio_gpu_pci.hpp"
#include "pci.h"
#include "memory/talloc.h"
#include "console/kio.h"
#include "ui/draw/draw.h"
#include "std/std.h"
#include "theme/theme.h"

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO          0x0108
#define VIRTIO_GPU_CMD_UPDATE_CURSOR            0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR              0x0301

#define VIRTIO_GPU_RESP_OK_NODATA               0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO         0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO          0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET               0x1103
#define VIRTIO_GPU_RESP_OK_EDID                 0x1104
#define VIRTIO_GPU_RESP_OK_RESOURCE_UUID        0x1105 
#define VIRTIO_GPU_RESP_OK_MAP_INFO             0x1106

#define VIRTIO_GPU_FLAG_FENCE   (1 << 0)

#define BPP 4

#define CONTROL_QUEUE 0
#define CURSOR_QUEUE 1

//TODO: format logs
VirtioGPUDriver* VirtioGPUDriver::try_init(gpu_size preferred_screen_size){
    VirtioGPUDriver* driver = new VirtioGPUDriver();
    if (driver->init(preferred_screen_size))
        return driver;
    delete driver;
    return nullptr;
}

bool VirtioGPUDriver::init(gpu_size preferred_screen_size){
    
    uint64_t addr = find_pci_device(VIRTIO_VENDOR, VIRTIO_GPU_ID);
    if (!addr){ 
        kprintf("[VIRTIO_GPU error] Virtio GPU not found");
        return false;
    }

    pci_enable_device(addr);

    uint64_t device_address, device_size;

    virtio_get_capabilities(&gpu_dev, addr, &device_address, &device_size);
    pci_register(device_address, device_size);
    if (!virtio_init_device(&gpu_dev)) {
        kprintf("[VIRTIO_GPU error] Failed initialization");
        return false;
    }

    kprintf("[VIRTIO_GPU] GPU initialized. Issuing commands");

    screen_size = get_display_info();

    kprintf("[VIRTIO_GPU] Display size %ix%i", screen_size.width,screen_size.height);
    if (screen_size.width == 0 || screen_size.height == 0){
        return false;
    }

    resource_id_counter = 0;
    
    framebuffer_size = screen_size.width * screen_size.height * BPP;
    framebuffer = (uintptr_t)kalloc(gpu_dev.memory_page, framebuffer_size, ALIGN_4KB, true, true);

    ctx = {
        .dirty_rects = {},
        .fb = (uint32_t*)framebuffer,
        .stride = screen_size.width * BPP,
        .width = screen_size.width,
        .height = screen_size.height,
        .dirty_count = 0,
        .full_redraw = 0,
    };

    get_capset();

    fb_resource_id = new_resource_id();
    
    if (!create_2d_resource(fb_resource_id, screen_size)) return false;
    
    if (!attach_backing(fb_resource_id, (sizedptr){framebuffer,framebuffer_size})) return false;

    if (scanout_found)
        set_scanout();
    else
        kprintf("[VIRTIO_GPU error] GPU did not return valid scanout data");

    return true;
}

uint32_t VirtioGPUDriver::new_resource_id(){
    return ++resource_id_counter;
}

#define VIRTIO_GPU_MAX_SCANOUTS 16 

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
    virtio_gpu_ctrl_hdr* cmd = (virtio_gpu_ctrl_hdr*)kalloc(gpu_dev.memory_page, sizeof(virtio_gpu_ctrl_hdr), ALIGN_4KB, true, true);
    cmd->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    cmd->flags = 0;
    cmd->fence_id = 0;
    cmd->ctx_id = 0;
    cmd->ring_idx = 0;
    cmd->padding[0] = 0;
    cmd->padding[1] = 0;
    cmd->padding[2] = 0;

    virtio_gpu_resp_display_info* resp = (virtio_gpu_resp_display_info*)kalloc(gpu_dev.memory_page, sizeof(virtio_gpu_resp_display_info), ALIGN_4KB, true, true);

    if (!virtio_send_3d(&gpu_dev, (uintptr_t)cmd, sizeof(virtio_gpu_ctrl_hdr), (uintptr_t)resp, sizeof(virtio_gpu_resp_display_info), VIRTQ_DESC_F_WRITE)){
        kfree((void*)cmd, sizeof(virtio_gpu_ctrl_hdr));
        kfree((void*)resp, sizeof(virtio_gpu_resp_display_info));
        return (gpu_size){0, 0};
    }

    if (resp->hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        kfree((void*)(void*)cmd, sizeof(virtio_gpu_ctrl_hdr));
        kfree((void*)resp, sizeof(virtio_gpu_resp_display_info));
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
    kfree((void*)cmd, sizeof(virtio_gpu_ctrl_hdr));
    kfree((void*)resp, sizeof(virtio_gpu_resp_display_info));
    return (gpu_size){0, 0};
}

bool VirtioGPUDriver::create_2d_resource(uint32_t resource_id, gpu_size size) {
    virtio_2d_resource* cmd = (virtio_2d_resource*)kalloc(gpu_dev.memory_page, sizeof(virtio_2d_resource), ALIGN_4KB, true, true);
    
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = VIRTIO_GPU_FLAG_FENCE;
    cmd->hdr.ctx_id = 0;
    cmd->hdr.ring_idx = 0;
    cmd->hdr.padding[0] = 0;
    cmd->hdr.padding[1] = 0;
    cmd->hdr.padding[2] = 0;
    cmd->resource_id = resource_id;
    cmd->format = 1; // VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM
    cmd->width = size.width;
    cmd->height = size.height;

    virtio_gpu_ctrl_hdr* resp = (virtio_gpu_ctrl_hdr*)kalloc(gpu_dev.memory_page, sizeof(virtio_gpu_ctrl_hdr), ALIGN_4KB, true, true);

    if (!virtio_send_3d(&gpu_dev, (uintptr_t)cmd, sizeof(virtio_2d_resource), (uintptr_t)resp, sizeof(virtio_gpu_ctrl_hdr), VIRTQ_DESC_F_WRITE)){
        kfree((void*)cmd, sizeof(virtio_2d_resource));
        kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));
        return false;
    }
    
    if (resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        kfree((void*)cmd, sizeof(virtio_2d_resource));
        kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));
        return false;
    }

    kfree((void*)cmd, sizeof(virtio_2d_resource));
    kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));

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

bool VirtioGPUDriver::attach_backing(uint32_t resource_id, sizedptr ptr) {
    virtio_backing_cmd* cmd = (virtio_backing_cmd*)kalloc(gpu_dev.memory_page, sizeof(virtio_backing_cmd), ALIGN_4KB, true, true);

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = VIRTIO_GPU_FLAG_FENCE;
    cmd->hdr.ctx_id = 0;
    cmd->hdr.ring_idx = 0;
    cmd->hdr.padding[0] = 0;
    cmd->hdr.padding[1] = 0;
    cmd->hdr.padding[2] = 0;
    cmd->resource_id = resource_id;
    cmd->nr_entries = 1;
    
    cmd->entries[0].addr = ptr.ptr;
    cmd->entries[0].length = ptr.size;
    cmd->entries[0].padding = 0;

    virtio_gpu_ctrl_hdr* resp = (virtio_gpu_ctrl_hdr*)kalloc(gpu_dev.memory_page, sizeof(virtio_gpu_ctrl_hdr), ALIGN_4KB, true, true);

    if (!virtio_send_2d(&gpu_dev, (uintptr_t)cmd, sizeof(*cmd), (uintptr_t)resp, sizeof(virtio_gpu_ctrl_hdr), VIRTQ_DESC_F_NEXT)){
        kfree((void*)cmd, sizeof(*cmd));
        kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));
        return false;
    }

    if (resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        kfree((void*)cmd, sizeof(virtio_backing_cmd));
        kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));
        return false;
    }

    kfree((void*)cmd, sizeof(virtio_backing_cmd));
    kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));
    return true;
}

typedef struct virtio_scanout_cmd {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
}__attribute__((packed)) virtio_scanout_cmd;

bool VirtioGPUDriver::set_scanout() {
    virtio_scanout_cmd* cmd = (virtio_scanout_cmd*)kalloc(gpu_dev.memory_page, sizeof(virtio_scanout_cmd), ALIGN_4KB, true, true);
    
    cmd->r.x = 0;
    cmd->r.y = 0;
    cmd->r.width = screen_size.width;
    cmd->r.height = screen_size.height;

    cmd->scanout_id = scanout_id;
    cmd->resource_id = fb_resource_id;

    cmd->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = 0;
    cmd->hdr.ctx_id = 0;
    cmd->hdr.ring_idx = 0;
    cmd->hdr.padding[0] = 0;
    cmd->hdr.padding[1] = 0;
    cmd->hdr.padding[2] = 0;

    virtio_gpu_ctrl_hdr* resp = (virtio_gpu_ctrl_hdr*)kalloc(gpu_dev.memory_page, sizeof(virtio_gpu_ctrl_hdr), ALIGN_4KB, true, true);

    if (!virtio_send_3d(&gpu_dev, (uintptr_t)cmd, sizeof(*cmd), (uintptr_t)resp, sizeof(virtio_gpu_ctrl_hdr), VIRTQ_DESC_F_WRITE)){
        kfree((void*)cmd, sizeof(virtio_scanout_cmd));
        kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));
        return false;
    }

    if (resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        kfree((void*)cmd, sizeof(virtio_scanout_cmd));
        kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));
        return false;
    }

    kfree((void*)cmd, sizeof(virtio_scanout_cmd));
    kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));
    return true;
}

typedef struct virtio_transfer_cmd {
    struct virtio_gpu_ctrl_hdr hdr; 
    struct virtio_rect rect; 
    uint64_t offset; 
    uint32_t resource_id; 
    uint32_t padding; 
}__attribute__((packed)) virtio_transfer_cmd;

bool VirtioGPUDriver::transfer_to_host(uint32_t resource_id, gpu_rect rect) {
    virtio_transfer_cmd* cmd = (virtio_transfer_cmd*)kalloc(gpu_dev.memory_page, sizeof(virtio_transfer_cmd), ALIGN_4KB, true, true);
    
    cmd->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd->hdr.fence_id = VIRTIO_GPU_FLAG_FENCE;
    cmd->resource_id = resource_id;
    cmd->rect.x = rect.point.x;
    cmd->rect.y = rect.point.y;
    cmd->rect.width = rect.size.width;
    cmd->rect.height = rect.size.height;

    virtio_gpu_ctrl_hdr* resp = (virtio_gpu_ctrl_hdr*)kalloc(gpu_dev.memory_page, sizeof(virtio_gpu_ctrl_hdr), ALIGN_4KB, true, true);

    if (!virtio_send_3d(&gpu_dev,(uintptr_t)cmd, sizeof(virtio_transfer_cmd), (uintptr_t)resp, sizeof(virtio_gpu_ctrl_hdr), VIRTQ_DESC_F_WRITE)){
        kfree((void*)cmd, sizeof(virtio_transfer_cmd));
        kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));
        return false;
    }

    if (resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        kfree((void*)cmd, sizeof(virtio_transfer_cmd));
        kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));
        return false;
    }

    kfree((void*)cmd, sizeof(virtio_transfer_cmd));
    kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));
    return true;
}

typedef struct virtio_flush_cmd {
    struct virtio_gpu_ctrl_hdr hdr; 
    struct virtio_rect rect; 
    uint32_t resource_id; 
    uint32_t padding; 
}__attribute__((packed)) virtio_flush_cmd;

void VirtioGPUDriver::flush() {

    if (ctx.full_redraw) {
        transfer_to_host(fb_resource_id, (gpu_rect){{0,0},{screen_size.width,screen_size.height}});
    } else {
        for (uint32_t i = 0; i < ctx.dirty_count; i++) {
            gpu_rect r = ctx.dirty_rects[i];
            transfer_to_host(fb_resource_id, r);
        }
    }
    
    virtio_flush_cmd* cmd = (virtio_flush_cmd*)kalloc(gpu_dev.memory_page, sizeof(virtio_flush_cmd), ALIGN_4KB, true, true);
    
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = 0;
    cmd->hdr.padding[0] = 0;
    cmd->hdr.padding[1] = 0;
    cmd->hdr.padding[2] = 0;
    cmd->resource_id = fb_resource_id;
    cmd->padding = 0;
    cmd->rect.x = 0;
    cmd->rect.y = 0;
    cmd->rect.width = screen_size.width;
    cmd->rect.height = screen_size.height;

    //TODO: 
    // ctx.dirty_count = 0;
    // ctx.full_redraw = false;

    virtio_gpu_ctrl_hdr* resp = (virtio_gpu_ctrl_hdr*)kalloc(gpu_dev.memory_page, sizeof(virtio_gpu_ctrl_hdr), ALIGN_4KB, true, true);

    if (!virtio_send_3d(&gpu_dev, (uintptr_t)cmd, sizeof(virtio_flush_cmd), (uintptr_t)resp, sizeof(virtio_gpu_ctrl_hdr), VIRTQ_DESC_F_WRITE)){
        kfree((void*)cmd, sizeof(virtio_flush_cmd));
        kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));
        return;
    }

    if (resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        kfree((void*)cmd, sizeof(virtio_flush_cmd));
        kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));
        return;
    }

    kfree((void*)cmd, sizeof(virtio_flush_cmd));
    kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));
    return;
}

struct virtio_gpu_get_capset_info { 
    struct virtio_gpu_ctrl_hdr hdr; 
    uint32_t capset_index; 
    uint32_t padding; 
}; 

struct virtio_gpu_resp_capset_info { 
    struct virtio_gpu_ctrl_hdr hdr; 
    uint32_t capset_id; 
    uint32_t capset_max_version; 
    uint32_t capset_max_size; 
    uint32_t padding; 
};

void VirtioGPUDriver::get_capset(){
    virtio_gpu_get_capset_info* cmd = (virtio_gpu_get_capset_info*)kalloc(gpu_dev.memory_page, sizeof(virtio_gpu_get_capset_info), ALIGN_4KB, true, true);
    
    cmd->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = 0;
    cmd->capset_index = 0;
    cmd->padding = 0;

    virtio_gpu_resp_capset_info* resp = (virtio_gpu_resp_capset_info*)kalloc(gpu_dev.memory_page, sizeof(virtio_gpu_resp_capset_info), ALIGN_4KB, true, true);

    if (!virtio_send_3d(&gpu_dev, (uintptr_t)cmd, sizeof(virtio_gpu_get_capset_info), (uintptr_t)resp, sizeof(virtio_gpu_resp_capset_info), VIRTQ_DESC_F_WRITE)){
        kprintf("Could not send command");
        kfree((void*)cmd, sizeof(virtio_gpu_get_capset_info));
        kfree((void*)resp, sizeof(virtio_gpu_resp_capset_info));
        return;
    }

    if (resp->hdr.type != VIRTIO_GPU_RESP_OK_CAPSET_INFO) {
        kprintf("Received wrong response");
        kfree((void*)cmd, sizeof(virtio_gpu_get_capset_info));
        kfree((void*)resp, sizeof(virtio_gpu_resp_capset_info));
        return;
    }

    kprintf("Capset 0's %i", resp->capset_id);

    kfree((void*)cmd, sizeof(virtio_gpu_get_capset_info));
    kfree((void*)resp, sizeof(virtio_gpu_resp_capset_info));
    return;
}

void VirtioGPUDriver::clear(uint32_t color) {
    fb_clear(&ctx, color);
}

void VirtioGPUDriver::draw_pixel(uint32_t x, uint32_t y, color color){
    fb_draw_pixel(&ctx, x, y, color);
}

void VirtioGPUDriver::fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, color color){
    fb_fill_rect(&ctx, x, y, width, height, color);
}

void VirtioGPUDriver::draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, color color){
    fb_draw_line(&ctx, x0, y0, x1, y1, color);
}

void VirtioGPUDriver::draw_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color){
    fb_draw_char(&ctx, x, y, c, scale, color);
}

void VirtioGPUDriver::draw_string(string s, uint32_t x, uint32_t y, uint32_t scale, uint32_t color){
    fb_draw_string(&ctx, s.data, x, y, scale, color);
}

uint32_t VirtioGPUDriver::get_char_size(uint32_t scale){
    return fb_get_char_size(scale);
}

gpu_size VirtioGPUDriver::get_screen_size(){
    return screen_size;
}

draw_ctx* VirtioGPUDriver::get_ctx(){
    return &ctx;
}

uint32_t VirtioGPUDriver::new_cursor(uint32_t color){
    uint32_t id = new_resource_id();
    size_t cursor_size = 64*64*BPP;
    create_2d_resource(id, {64,64});
    uint32_t *cursor = (uint32_t*)kalloc(gpu_dev.memory_page, cursor_size, ALIGN_4KB, true, true);
    draw_ctx ctx = {{},cursor, 64 * BPP, 64, 64, 0,0};
    fb_draw_cursor(&ctx, color);
    attach_backing(id, (sizedptr){(uintptr_t)cursor, cursor_size});
    transfer_to_host(id, {{0,0},{64,64}});
    return id;
}

void VirtioGPUDriver::setup_cursor()
{
    cursor_pressed_resource_id = new_cursor(CURSOR_COLOR_SELECTED);
    cursor_unpressed_resource_id = new_cursor(CURSOR_COLOR_DESELECTED);
    set_cursor_pressed(false);
}   

void VirtioGPUDriver::update_cursor(uint32_t x, uint32_t y, bool full)
{
    select_queue(&gpu_dev, CURSOR_QUEUE);

    if (!cursor_cmd) cursor_cmd = (virtio_gpu_update_cursor*)kalloc(gpu_dev.memory_page, sizeof(virtio_gpu_update_cursor), ALIGN_4KB, true, true);
    
    cursor_cmd->hdr.type = full ? VIRTIO_GPU_CMD_UPDATE_CURSOR : VIRTIO_GPU_CMD_MOVE_CURSOR;
    cursor_cmd->pos.scanout_id = scanout_id;
    cursor_cmd->pos.x = x;
    cursor_cmd->pos.y = y;
    cursor_cmd->resource_id = cursor_resource_id;

    virtio_gpu_ctrl_hdr* resp = (virtio_gpu_ctrl_hdr*)kalloc(gpu_dev.memory_page, sizeof(virtio_gpu_ctrl_hdr), ALIGN_4KB, true, true);

    if (!virtio_send_3d(&gpu_dev, (uintptr_t)cursor_cmd, sizeof(virtio_gpu_update_cursor), (uintptr_t)resp, sizeof(virtio_gpu_ctrl_hdr), VIRTQ_DESC_F_WRITE)){
        select_queue(&gpu_dev, CONTROL_QUEUE);
        kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));
        return;
    }

    select_queue(&gpu_dev, CONTROL_QUEUE);
    
    if (resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));
        return;
    }

    kfree((void*)resp, sizeof(virtio_gpu_ctrl_hdr));
}

void VirtioGPUDriver::set_cursor_pressed(bool pressed){
    cursor_resource_id = pressed ? cursor_pressed_resource_id : cursor_unpressed_resource_id;
}

void VirtioGPUDriver::create_window(uint32_t x, uint32_t y, uint32_t width, uint32_t height, draw_ctx *new_ctx){
    // TODO: use this once we can ensure we can alloc continuous.
    // new_ctx->fb = (uint32_t*)palloc(width * height * BPP, false, false, true);
    new_ctx->fb = (uint32_t*)kalloc(gpu_dev.memory_page, width * height * BPP, ALIGN_4KB, MEM_PRIV_SHARED, false);
    new_ctx->width = width;
    new_ctx->height = height;
    new_ctx->stride = width * BPP;
}