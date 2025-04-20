#include "console/kio.h"
#include "ram_e.h"
#include "pci.h"

///

#define VIRTIO_STATUS_RESET         0x0
#define VIRTIO_STATUS_ACKNOWLEDGE   0x1
#define VIRTIO_STATUS_DRIVER        0x2
#define VIRTIO_STATUS_DRIVER_OK     0x4
#define VIRTIO_STATUS_FEATURES_OK   0x8
#define VIRTIO_STATUS_FAILED        0x80

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0102
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0103
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0104
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106

#define VIRTIO_PCI_CAP_COMMON_CFG        1
#define VIRTIO_PCI_CAP_NOTIFY_CFG        2
#define VIRTIO_PCI_CAP_ISR_CFG           3
#define VIRTIO_PCI_CAP_DEVICE_CFG        4
#define VIRTIO_PCI_CAP_PCI_CFG           5
#define VIRTIO_PCI_CAP_VENDOR_CFG        9

#define VENDOR_ID 0x1AF4
#define DEVICE_ID_BASE 0x1040
#define GPU_DEVICE_ID 0x10

#define VIRTIO_GPU_MAX_SCANOUTS 16 

#define VIRTQ_DESC_F_WRITE 2

#define framebuffer_size display_width * display_height * (FRAMEBUFFER_BPP/8)

struct virtio_pci_cap {
    uint8_t cap_vndr;
    uint8_t cap_next;
    uint8_t cap_len;
    uint8_t cfg_type;
    uint8_t bar;
    uint8_t id;
    uint8_t padding[2];
    uint32_t offset;
    uint32_t length;
};

struct virtio_pci_common_cfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t device_status;
    uint8_t config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
    uint16_t queue_notify_data;
    uint16_t queue_reset;
} __attribute__((packed));

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
};

struct virtio_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one {
        uint32_t enabled;
        uint32_t flags;
        uint32_t x;
        uint32_t y;
        uint32_t width;
        uint32_t height;
    } pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[128];
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[128];
} __attribute__((packed));

#define VIRTQ_DESC_F_NEXT 1

static uint64_t VIRTQUEUE_BASE;
static uint64_t VIRTQUEUE_AVAIL;
static uint64_t VIRTQUEUE_USED;

static uint64_t VIRTQUEUE_CMD;
static uint64_t VIRTQUEUE_RESP;
static uint64_t VIRTQUEUE_DISP_INFO;

/// Capabilities & GPU init

volatile struct virtio_pci_common_cfg* common_cfg = 0;
volatile uint8_t* notify_cfg = 0;
volatile uint8_t* device_cfg = 0;
volatile uint8_t* isr_cfg = 0;
uint32_t notify_off_multiplier;

#define GPU_RESOURCE_ID 1
static uint32_t display_width = 800;
static uint32_t display_height = 600;
static uint64_t framebuffer_memory = 0x0;
static uint32_t scanout_id = 0;
static bool scanout_found = false;
static uint32_t default_width;
static uint32_t default_height;
#define FRAMEBUFFER_BPP 32

uint64_t vgp_setup_bars(uint64_t base, uint8_t bar) {
    uint64_t bar_addr = pci_get_bar(base, 0x10, bar);
    printf("Setting up GPU BAR@%h FROM BAR %i", bar_addr, bar);

    write32(bar_addr, 0xFFFFFFFF);
    uint64_t bar_val = read32(bar_addr);

    if (bar_val == 0 || bar_val == 0xFFFFFFFF) {
        printf("BAR size probing failed");
        return 0;
    }

    uint64_t size = ((uint64_t)(~(bar_val & ~0xF)) + 1);
    printf("Calculated BAR size: %h", size);

    uint64_t config_base = 0x10010000;
    write32(bar_addr, config_base & 0xFFFFFFFF);

    bar_val = read32(bar_addr);

    printf("FINAL BAR value: %h", bar_val);

    uint32_t cmd = read32(base + 0x4);
    cmd |= 0x2;
    write32(base + 0x4, cmd);

    return (bar_val & ~0xF);
}

void vgp_start() {
    printf("Starting VirtIO GPU initialization");

    common_cfg->device_status = 0;
    while (common_cfg->device_status != 0);

    printf("Device reset");

    common_cfg->device_status |= VIRTIO_STATUS_ACKNOWLEDGE;
    printf("ACK sent");

    common_cfg->device_status |= VIRTIO_STATUS_DRIVER;
    printf("DRIVER sent");

    common_cfg->device_feature_select = 0;
    uint32_t features = common_cfg->device_feature;

    printf("Features received %h", features);

    common_cfg->driver_feature_select = 0;
    common_cfg->driver_feature = features;

    common_cfg->device_status |= VIRTIO_STATUS_FEATURES_OK;

    if (!(common_cfg->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        printf("FEATURES_OK not accepted, device unusable");
        return;
    }

    common_cfg->queue_select = 0;
    uint32_t queue_size = common_cfg->queue_size;

    printf("Queue size: %h", queue_size);

    common_cfg->queue_size = queue_size;

    VIRTQUEUE_BASE = palloc(4096);
    VIRTQUEUE_AVAIL = palloc(4096);
    VIRTQUEUE_USED = palloc(4096);
    VIRTQUEUE_CMD = palloc(4096);
    VIRTQUEUE_RESP = palloc(4096);
    VIRTQUEUE_DISP_INFO = palloc(sizeof(struct virtio_gpu_resp_display_info));

    common_cfg->queue_desc = VIRTQUEUE_BASE;
    common_cfg->queue_driver = VIRTQUEUE_AVAIL;
    common_cfg->queue_device = VIRTQUEUE_USED;
    common_cfg->queue_enable = 1;

    common_cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;

    printf("VirtIO GPU initialization complete");
}

volatile struct virtio_pci_cap* vgp_get_capabilities(uint64_t address) {
    uint64_t offset = read32(address + 0x34);

    while (offset != 0x0) {
        uint64_t cap_address = address + offset;
        volatile struct virtio_pci_cap* cap = (volatile struct virtio_pci_cap*)(uintptr_t)(cap_address);

        printf("Inspecting@%h = %h (%h + %h) TYPE %h -> %h",cap_address, cap->cap_vndr, cap->bar, cap->offset, cap->cfg_type, cap->cap_next);

        uint64_t target = pci_get_bar(address, 0x10, cap->bar);
        uint64_t val = read32(target) & ~0xF;

        if (cap->cap_vndr == 0x9) {
            if (cap->cfg_type < VIRTIO_PCI_CAP_PCI_CFG && val == 0) {
                val = vgp_setup_bars(address, cap->bar);
            }

            if (cap->cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) {
                printf("Found common config %h", val + cap->offset);
                common_cfg = (volatile struct virtio_pci_common_cfg*)(uintptr_t)(val + cap->offset);
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                printf("Found notify config %h", val + cap->offset);
                notify_cfg = (volatile uint8_t*)(uintptr_t)(val + cap->offset);
                notify_off_multiplier = *(volatile uint32_t*)(uintptr_t)(cap_address + sizeof(struct virtio_pci_cap));
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) {
                printf("Found device config %h", val + cap->offset);
                device_cfg = (volatile uint8_t*)(uintptr_t)(val + cap->offset);
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_ISR_CFG) {
                printf("Found ISR config %h", val + cap->offset);
                isr_cfg = (volatile uint8_t*)(uintptr_t)(val + cap->offset);
            }
        }

        offset = cap->cap_next;
    }

    return 0;
}

// Screen render functions

void vgp_send_command(uint64_t cmd_addr, uint32_t cmd_size, uint64_t resp_addr, uint32_t resp_size, uint64_t notify_base, uint32_t notify_multiplier, uint8_t flags) {

    volatile struct virtq_desc* desc = (volatile struct virtq_desc*)(uintptr_t)(common_cfg->queue_desc);
    volatile struct virtq_avail* avail = (volatile struct virtq_avail*)(uintptr_t)(common_cfg->queue_driver);
    volatile struct virtq_used* used = (volatile struct virtq_used*)(uintptr_t)(common_cfg->queue_device);

    desc[0].addr = cmd_addr;
    desc[0].len = cmd_size;
    desc[0].flags = flags;
    desc[0].next = 1;

    desc[1].addr = resp_addr;
    desc[1].len = resp_size;
    desc[1].flags = VIRTQ_DESC_F_WRITE; 
    desc[1].next = 0;

    avail->ring[avail->idx % 128] = 0;
    avail->idx++;

    *(volatile uint16_t*)(uintptr_t)(notify_base + notify_multiplier * 0) = 0;

    uint16_t last_used_idx = used->idx;
    while (last_used_idx == used->idx);
    last_used_idx = used->idx;
}

bool vgp_get_display_info(){
    volatile struct virtio_gpu_ctrl_hdr* cmd = (volatile struct virtio_gpu_ctrl_hdr*)(uintptr_t)(VIRTQUEUE_CMD);
    cmd->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    cmd->flags = 0;
    cmd->fence_id = 0;
    cmd->ctx_id = 0;
    cmd->ring_idx = 0;
    cmd->padding[0] = 0;
    cmd->padding[1] = 0;
    cmd->padding[2] = 0;

    printf("Command prepared");

    vgp_send_command((uint64_t)cmd, sizeof(struct virtio_gpu_ctrl_hdr), VIRTQUEUE_DISP_INFO, sizeof(struct virtio_gpu_resp_display_info), (uint64_t)notify_cfg, notify_off_multiplier, 0);

    volatile struct virtio_gpu_resp_display_info* resp = (volatile struct virtio_gpu_resp_display_info*)(uintptr_t)(VIRTQUEUE_DISP_INFO);

    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++){
        
        printf("Scanout %i: enabled=%i size=%ix%i",i,resp->pmodes[i].enabled, resp->pmodes[i].width, resp->pmodes[i].height);

        if (resp->pmodes[i].enabled) {
            printf("Found a valid display: %ix%i",resp->pmodes[i].width,resp->pmodes[i].height);
            display_width = resp->pmodes[i].width;
            display_height = resp->pmodes[i].height;
            scanout_id = i;
            scanout_found = true;
            return true;
        }
    }

    printf("Display not enabled yet. Using default but not allowing scanout");
    resp->pmodes[0].width = default_width;
    resp->pmodes[0].height = default_height;
    scanout_found = false;
    return false;
}

void vgp_create_2d_resource() {
    volatile struct {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t resource_id;
        uint32_t format;
        uint32_t width;
        uint32_t height;
    } *cmd = (volatile void*)(uintptr_t)(VIRTQUEUE_CMD);
    
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
    cmd->width = display_width;
    cmd->height = display_height;
    
    vgp_send_command((uint64_t)cmd, sizeof(*cmd), VIRTQUEUE_RESP, sizeof(struct virtio_gpu_ctrl_hdr), (uint64_t)notify_cfg, notify_off_multiplier, VIRTQ_DESC_F_NEXT);

    asm volatile("" ::: "memory");

    volatile struct virtio_gpu_ctrl_hdr* resp = (volatile void*)(uintptr_t)(VIRTQUEUE_RESP);

    printf("Response type: %h flags: %h", resp->type, resp->flags);
    
    if (resp->type == 0x1100) {
        printf("RESOURCE_CREATE_2D OK");
    } else {
        printf("RESOURCE_CREATE_2D ERROR: %h", resp->type);
    }
}

void vgp_attach_backing() {
    volatile uint8_t* ptr = (volatile uint8_t*)(uintptr_t)VIRTQUEUE_CMD;

    volatile struct {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t resource_id;
        uint32_t nr_entries;
    }__attribute__((packed)) *cmd = (volatile void*)ptr;

    volatile struct {
        uint64_t addr;
        uint32_t length;
        uint32_t padding;
    }__attribute__((packed)) *entry = (volatile void*)(ptr + sizeof(*cmd));

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

    printf("Attach framebuffer addr: %h, size: %i",framebuffer_memory,framebuffer_size);

    entry->addr = framebuffer_memory;
    entry->length = framebuffer_size;
    entry->padding = 0;

    vgp_send_command((uint64_t)cmd, sizeof(*cmd) + sizeof(*entry), VIRTQUEUE_RESP, sizeof(struct virtio_gpu_ctrl_hdr), (uint64_t)notify_cfg, notify_off_multiplier, VIRTQ_DESC_F_NEXT);

    asm volatile("" ::: "memory");

    volatile struct virtio_gpu_ctrl_hdr* resp = (volatile void*)(uintptr_t)(VIRTQUEUE_RESP);

    printf("Response type: %h flags: %h", resp->type, resp->flags);

    if (resp->type == 0x1100) {
        printf("RESOURCE_ATTACH_BACKING OK");
    } else {
        printf("RESOURCE_ATTACH_BACKING ERROR: %h", resp->type);
    }
}

void vgp_set_scanout() {
    volatile struct {
        struct virtio_gpu_ctrl_hdr hdr;
        struct virtio_rect r;
        uint32_t scanout_id;
        uint32_t resource_id;
    }__attribute__((packed)) *cmd = (volatile void*)(uintptr_t)VIRTQUEUE_CMD;

    
    cmd->r.x = 0;
    cmd->r.y = 0;
    cmd->r.width = display_width;
    cmd->r.height = display_height;

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


    vgp_send_command((uint64_t)cmd, sizeof(*cmd), VIRTQUEUE_RESP, sizeof(struct virtio_gpu_ctrl_hdr), (uint64_t)notify_cfg, notify_off_multiplier, VIRTQ_DESC_F_NEXT);

    asm volatile("" ::: "memory");

    volatile struct virtio_gpu_ctrl_hdr* resp = (volatile void*)(uintptr_t)VIRTQUEUE_RESP;

    printf("Response type: %h flags: %h", resp->type, resp->flags);

    if (resp->type == 0x1100) {
        printf("SCANOUT OK");
    } else {
        printf("SCANOUT ERROR: %h", resp->type);
    }
}

void vgp_transfer_to_host() {
    volatile struct {
        uint32_t type;
        uint32_t flags;
        uint64_t fence_id;
        uint32_t resource_id;
        uint32_t rsvd;
        uint32_t x;
        uint32_t y;
        uint32_t width;
        uint32_t height;
    } *cmd = (volatile void*)(uintptr_t)(VIRTQUEUE_CMD);
    
    cmd->type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd->flags = 0;
    cmd->fence_id = 0;
    cmd->resource_id = GPU_RESOURCE_ID;
    cmd->rsvd = 0;
    cmd->x = 0;
    cmd->y = 0;
    cmd->width = display_width;
    cmd->height = display_height;
    
    vgp_send_command((uint64_t)cmd, sizeof(*cmd), VIRTQUEUE_RESP, sizeof(struct virtio_gpu_ctrl_hdr), (uint64_t)notify_cfg, notify_off_multiplier, VIRTQ_DESC_F_NEXT);

    asm volatile("" ::: "memory");

    volatile struct virtio_gpu_ctrl_hdr* resp = (volatile void*)(uintptr_t)(VIRTQUEUE_RESP);

    printf("Response type: %h flags: %h", resp->type, resp->flags);
    
    if (resp->type == 0x1100) {
        printf("TRANSFER_TO_HOST OK");
    } else {
        printf("TRANSFER_TO_HOST ERROR: %h", resp->type);
    }
}

void vgp_flush() {

    vgp_transfer_to_host();

    volatile struct {
        uint32_t type;
        uint32_t flags;
        uint64_t fence_id;
        uint32_t resource_id;
        uint32_t rsvd;
    } *cmd = (volatile void*)(uintptr_t)(VIRTQUEUE_CMD);
    
    cmd->type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd->flags = 0;
    cmd->fence_id = 0;
    cmd->resource_id = GPU_RESOURCE_ID;
    cmd->rsvd = 0;
    
    vgp_send_command((uint64_t)cmd, sizeof(*cmd), VIRTQUEUE_RESP, sizeof(struct virtio_gpu_ctrl_hdr), (uint64_t)notify_cfg, notify_off_multiplier, VIRTQ_DESC_F_NEXT);

    asm volatile("" ::: "memory");

    volatile struct virtio_gpu_ctrl_hdr* resp = (volatile void*)(uintptr_t)(VIRTQUEUE_RESP);
    
    printf("Response type: %h flags: %h", resp->type, resp->flags);

    if (resp->type == 0x1100) {
        printf("FLUSH OK");
    } else {
        printf("FLUSH ERROR: %h", resp->type);
    }
}

void vgp_clear(uint32_t color) {

    printf("Clear screen");

    volatile uint32_t* fb = (volatile uint32_t*)framebuffer_memory;
    for (uint32_t i = 0; i < display_width * display_height; i++) {
        fb[i] = color;
    }

    vgp_transfer_to_host();
    vgp_flush();
}

void vgp_draw_pixel(uint32_t x, uint32_t y, uint32_t color){

}

void vgp_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color){

}

void vgp_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color){

}

void vgp_draw_char(uint32_t x, uint32_t y, char c, uint32_t color) {
    
}

bool vgp_init(uint32_t width, uint32_t height) {
    uint64_t address = find_pci_device(VENDOR_ID, DEVICE_ID_BASE + GPU_DEVICE_ID);

    default_width = width;
    default_height = height;

    if (address > 0) {
        printf("VGP GPU detected at %h",address);

        printf("Initializing GPU...");

        vgp_get_capabilities(address);
        vgp_start();

        printf("GPU initialized. Issuing commands");

        vgp_get_display_info();

        framebuffer_memory = palloc(framebuffer_size);

        vgp_create_2d_resource();

        vgp_attach_backing();

        vgp_transfer_to_host();

        vgp_flush();

        if (scanout_found)
            vgp_set_scanout();
        else 
            printf("GPU did not return valid scanout data");

        printf("GPU ready");

        return true;
    }

    return false;
}