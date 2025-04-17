#include "framebuffer.h"
#include "uart.h"
#include "mmio.h"
#include "pci.h"

#define VIRTIO_STATUS_RESET         0x0
#define VIRTIO_STATUS_ACKNOWLEDGE   0x1
#define VIRTIO_STATUS_DRIVER        0x2
#define VIRTIO_STATUS_DRIVER_OK     0x4
#define VIRTIO_STATUS_FEATURES_OK   0x8
#define VIRTIO_STATUS_FAILED        0x80

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100

#define VIRTIO_PCI_CAP_COMMON_CFG        1
#define VIRTIO_PCI_CAP_NOTIFY_CFG        2
#define VIRTIO_PCI_CAP_ISR_CFG           3
#define VIRTIO_PCI_CAP_DEVICE_CFG        4
#define VIRTIO_PCI_CAP_PCI_CFG           5
#define VIRTIO_PCI_CAP_VENDOR_CFG        9

#define VENDOR_ID 0x1AF4
#define DEVICE_ID_BASE 0x1040
#define GPU_DEVICE_ID 0x10

#define VIRTQUEUE_BASE 0x41000000

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
    uint32_t padding;
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

volatile struct virtio_pci_common_cfg* common_cfg = 0;
volatile uint8_t* notify_cfg = 0;
volatile uint8_t* device_cfg = 0;
volatile uint8_t* isr_cfg = 0;
uint32_t notify_off_multiplier;

uint64_t setup_gpu_bars(uint64_t base, uint8_t bar) {
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

void gpu_send_command(uint64_t cmd_addr, uint64_t notify_base, uint32_t notify_multiplier) {

    uart_puts("New command\n");

    volatile struct virtq_desc* desc = (volatile struct virtq_desc*)(uintptr_t)(VIRTQUEUE_BASE + 1000);
    volatile struct virtq_avail* avail = (volatile struct virtq_avail*)(uintptr_t)(VIRTQUEUE_BASE + 2000);
    volatile struct virtq_used* used = (volatile struct virtq_used*)(uintptr_t)(VIRTQUEUE_BASE + 3000);

    desc[0].addr = cmd_addr;
    desc[0].len = sizeof(struct virtio_gpu_ctrl_hdr);
    desc[0].flags = 0;
    desc[0].next = 0;

    avail->ring[avail->idx % 128] = 0;
    avail->idx++;

    *(volatile uint16_t*)(uintptr_t)(notify_base + notify_multiplier * 0) = 0;

    while (used->idx == 0);

    uart_puts("Command issued\n");
}

void virtio_gpu_init(uint64_t base_addr) {
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

    common_cfg->queue_desc = VIRTQUEUE_BASE + 1000;
    common_cfg->queue_driver = VIRTQUEUE_BASE + 2000;
    common_cfg->queue_device = VIRTQUEUE_BASE + 3000;
    common_cfg->queue_enable = 1;

    common_cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;

    uart_puts("VirtIO GPU initialization complete\n");
}

volatile struct virtio_pci_cap* get_capabilities(uint64_t address) {
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
                val = setup_gpu_bars(address, cap->bar);
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

void gpu_init() {
    uint64_t mmio_base;
    uint64_t address = find_pci_device(VENDOR_ID, DEVICE_ID_BASE + GPU_DEVICE_ID, &mmio_base);

    if (address > 0) {
        uart_puts("Virtio GPU detected at ");
        uart_puthex(address);
        uart_putc('\n');

        uart_puts("Initializing GPU...\n");

        get_capabilities(address);
        virtio_gpu_init(address);

        uart_puts("GPU initialized. Issuing commands\n");


        uart_puts("GPU ready\n");
    }
}