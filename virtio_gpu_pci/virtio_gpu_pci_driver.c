#include "../uart.h"
#include "../mmio.h"
#include "../pci.h"

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
#define FRAMEBUFFER_BPP 32

uint64_t vgp_setup_bars(uint64_t base, uint8_t bar) {
    uint64_t bar_addr = pci_get_bar(base, 0x10, bar);
    uart_puts("Setting up GPU BAR@");
    uart_puthex(bar_addr);
    uart_puts(" FROM BAR ");
    uart_puthex(bar);
    uart_putc('\n');

    write32(bar_addr, 0xFFFFFFFF);
    uint64_t bar_val = read32(bar_addr);

    if (bar_val == 0 || bar_val == 0xFFFFFFFF) {
        uart_puts("BAR size probing failed\n");
        return 0;
    }

    uint64_t size = ((uint64_t)(~(bar_val & ~0xF)) + 1);
    uart_puts("Calculated BAR size: ");
    uart_puthex(size);
    uart_putc('\n');

    uint64_t mmio_base = 0x10010000;
    write32(bar_addr, mmio_base & 0xFFFFFFFF);

    bar_val = read32(bar_addr);

    uart_puts("FINAL BAR value: ");
    uart_puthex(bar_val);
    uart_putc('\n');

    uint32_t cmd = read32(base + 0x4);
    cmd |= 0x2;
    write32(base + 0x4, cmd);

    return (bar_val & ~0xF);
}

void vgp_start() {
    uart_puts("Starting VirtIO GPU initialization\n");

    common_cfg->device_status = 0;
    while (common_cfg->device_status != 0);

    uart_puts("Device reset\n");

    common_cfg->device_status |= VIRTIO_STATUS_ACKNOWLEDGE;
    uart_puts("ACK sent\n");

    common_cfg->device_status |= VIRTIO_STATUS_DRIVER;
    uart_puts("DRIVER sent\n");

    common_cfg->device_feature_select = 0;
    uint32_t features = common_cfg->device_feature;

    uart_puts("Features received ");
    uart_puthex(features);
    uart_putc('\n');

    common_cfg->driver_feature_select = 0;
    common_cfg->driver_feature = features;

    common_cfg->device_status |= VIRTIO_STATUS_FEATURES_OK;

    if (!(common_cfg->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        uart_puts("FEATURES_OK not accepted, device unusable\n");
        return;
    }

    common_cfg->queue_select = 0;
    uint32_t queue_size = common_cfg->queue_size;

    uart_puts("Queue size: ");
    uart_puthex(queue_size);
    uart_putc('\n');

    common_cfg->queue_size = queue_size;

    VIRTQUEUE_BASE = alloc(4096);
    VIRTQUEUE_AVAIL = alloc(4096);
    VIRTQUEUE_USED = alloc(4096);
    VIRTQUEUE_CMD = alloc(4096);
    VIRTQUEUE_RESP = alloc(4096);
    VIRTQUEUE_DISP_INFO = alloc(sizeof(struct virtio_gpu_resp_display_info));

    common_cfg->queue_desc = VIRTQUEUE_BASE;
    common_cfg->queue_driver = VIRTQUEUE_AVAIL;
    common_cfg->queue_device = VIRTQUEUE_USED;
    common_cfg->queue_enable = 1;

    common_cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;

    uart_puts("VirtIO GPU initialization complete\n");
}

volatile struct virtio_pci_cap* vgp_get_capabilities(uint64_t address) {
    uint64_t offset = read32(address + 0x34);

    while (offset != 0x0) {
        uint64_t cap_address = address + offset;
        volatile struct virtio_pci_cap* cap = (volatile struct virtio_pci_cap*)(uintptr_t)(cap_address);

        uart_puts("Inspecting@");
        uart_puthex(cap_address);
        uart_puts(" = ");
        uart_puthex(cap->cap_vndr);
        uart_puts(" (");
        uart_puthex(cap->bar);
        uart_puts(" + ");
        uart_puthex(cap->offset);
        uart_puts(") TYPE ");
        uart_puthex(cap->cfg_type);
        uart_puts(" -> ");
        uart_puthex(cap->cap_next);
        uart_putc('\n');

        uint64_t target = pci_get_bar(address, 0x10, cap->bar);
        uint64_t val = read32(target) & ~0xF;

        if (cap->cap_vndr == 0x9) {
            if (cap->cfg_type < VIRTIO_PCI_CAP_PCI_CFG && val == 0) {
                val = vgp_setup_bars(address, cap->bar);
            }

            if (cap->cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) {
                uart_puts("FOUND COMMON CONFIG @");
                uart_puthex(val + cap->offset);
                uart_putc('\n');
                common_cfg = (volatile struct virtio_pci_common_cfg*)(uintptr_t)(val + cap->offset);
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                uart_puts("FOUND NOTIFY CONFIG @");
                uart_puthex(val + cap->offset);
                uart_putc('\n');
                notify_cfg = (volatile uint8_t*)(uintptr_t)(val + cap->offset);
                notify_off_multiplier = *(volatile uint32_t*)(uintptr_t)(cap_address + sizeof(struct virtio_pci_cap));
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) {
                uart_puts("FOUND DEVICE CONFIG @");
                uart_puthex(val + cap->offset);
                uart_putc('\n');
                device_cfg = (volatile uint8_t*)(uintptr_t)(val + cap->offset);
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_ISR_CFG) {
                uart_puts("FOUND ISR CONFIG @");
                uart_puthex(val + cap->offset);
                uart_putc('\n');
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

    uart_puts("Command prepared\n");

    vgp_send_command((uint64_t)cmd, sizeof(struct virtio_gpu_ctrl_hdr), VIRTQUEUE_DISP_INFO, sizeof(struct virtio_gpu_resp_display_info), (uint64_t)notify_cfg, notify_off_multiplier, 0);

    volatile struct virtio_gpu_resp_display_info* resp = (volatile struct virtio_gpu_resp_display_info*)(uintptr_t)(VIRTQUEUE_DISP_INFO);

    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++){
        
        uart_puts("Scanout ");
        uart_puthex(i);
        uart_puts(": enabled=");
        uart_puthex(resp->pmodes[i].enabled);
        uart_puts(" size=");
        uart_puthex(resp->pmodes[i].width);
        uart_putc('x');
        uart_puthex(resp->pmodes[i].height);
        uart_putc('\n');

        if (resp->pmodes[i].enabled) {
            uart_puts("FOUND A VALID DISPLAY: ");
            uart_puthex(resp->pmodes[i].width);
            uart_putc('x');
            uart_puthex(resp->pmodes[i].height);
            uart_putc('\n');
            display_width = resp->pmodes[i].width;
            display_height = resp->pmodes[i].height;
            scanout_id = i;
            scanout_found = true;
            return true;
        }
    }

    uart_puts("Display not enabled yet. Using default but not allowing scanout\n");
    resp->pmodes[0].width = 1024;
    resp->pmodes[0].height = 768;
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

    uart_puts("Response type: ");
    uart_puthex(resp->type);
    uart_puts(" flags: ");
    uart_puthex(resp->flags);
    uart_putc('\n');
    
    if (resp->type == 0x1100) {
        uart_puts("RESOURCE_CREATE_2D OK\n");
    } else {
        uart_puts("RESOURCE_CREATE_2D ERROR: ");
        uart_puthex(resp->type);
        uart_putc('\n');
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

    uart_puts("Attach framebuffer addr: ");
    uart_puthex(framebuffer_memory);
    uart_puts(", size: ");
    uart_puthex(framebuffer_size);
    uart_putc('\n');

    entry->addr = framebuffer_memory;
    entry->length = framebuffer_size;
    entry->padding = 0;

    vgp_send_command((uint64_t)cmd, sizeof(*cmd) + sizeof(*entry), VIRTQUEUE_RESP, sizeof(struct virtio_gpu_ctrl_hdr), (uint64_t)notify_cfg, notify_off_multiplier, VIRTQ_DESC_F_NEXT);

    asm volatile("" ::: "memory");

    volatile struct virtio_gpu_ctrl_hdr* resp = (volatile void*)(uintptr_t)(VIRTQUEUE_RESP);

    uart_puts("Response type: ");
    uart_puthex(resp->type);
    uart_puts(" flags: ");
    uart_puthex(resp->flags);
    uart_putc('\n');

    if (resp->type == 0x1100) {
        uart_puts("RESOURCE_ATTACH_BACKING OK\n");
    } else {
        uart_puts("RESOURCE_ATTACH_BACKING ERROR: ");
        uart_puthex(resp->type);
        uart_putc('\n');
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

    uart_puts("Response type: ");
    uart_puthex(resp->type);
    uart_puts(" flags: ");
    uart_puthex(resp->flags);
    uart_putc('\n');

    if (resp->type == 0x1100) {
        uart_puts("SCANOUT OK\n");
    } else {
        uart_puts("SCANOUT ERROR:\n");
        uart_puthex(resp->type);
        uart_putc('\n');
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

    uart_puts("Response type: ");
    uart_puthex(resp->type);
    uart_puts(" flags: ");
    uart_puthex(resp->flags);
    uart_putc('\n');
    
    if (resp->type == 0x1100) {
        uart_puts("TRANSFER OK\n");
    } else {
        uart_puts("TRANSFER ERROR: ");
        uart_puthex(resp->type);
        uart_putc('\n');
    }
}

void vgp_flush() {
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
    
    uart_puts("Response type: ");
    uart_puthex(resp->type);
    uart_puts(" flags: ");
    uart_puthex(resp->flags);
    uart_putc('\n');

    if (resp->type == 0x1100) {
        uart_puts("FLUSH OK\n");
    } else {
        uart_puts("FLUSH ERROR: ");
        uart_puthex(resp->type);
        uart_putc('\n');
    }
}

void vgp_clear(uint32_t color) {

    uart_puts("Clear screen\n");

    volatile uint32_t* fb = (volatile uint32_t*)framebuffer_memory;
    for (uint32_t i = 0; i < display_width * display_height; i++) {
        fb[i] = color;
    }

    vgp_transfer_to_host();
    vgp_flush();
}

bool vgp_init() {
    uint64_t mmio_base;
    uint64_t address = find_pci_device(VENDOR_ID, DEVICE_ID_BASE + GPU_DEVICE_ID, &mmio_base);

    if (address > 0) {
        uart_puts("VGP GPU detected at ");
        uart_puthex(address);
        uart_putc('\n');

        uart_puts("Initializing GPU...\n");

        vgp_get_capabilities(address);
        vgp_start();

        uart_puts("GPU initialized. Issuing commands\n");

        vgp_get_display_info();

        framebuffer_memory = alloc(framebuffer_size);

        vgp_create_2d_resource();

        vgp_attach_backing();

        vgp_transfer_to_host();

        vgp_flush();

        if (scanout_found)
            vgp_set_scanout();
        else 
            uart_puts("GPU did not return valid scanout data\n");

        uart_puts("GPU ready\n");

        return true;
    }

    return false;
}