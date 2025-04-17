#include "framebuffer.h"
#include "uart.h"
#include "mmio.h"
#include "pci.h"

// #define VIRTIO_MMIO_MAGIC_VALUE     0x000
// #define VIRTIO_MMIO_VERSION         0x004
// #define VIRTIO_MMIO_DEVICE_ID       0x008
// #define VIRTIO_MMIO_VENDOR_ID       0x00c
// #define VIRTIO_MMIO_STATUS          0x070
// #define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034
// #define VIRTIO_MMIO_QUEUE_PFN       0x040
// #define VIRTIO_MMIO_QUEUE_READY     0x044
// #define VIRTIO_MMIO_QUEUE_NOTIFY    0x050
// #define VIRTIO_MMIO_INTERRUPT_ACK   0x064
// #define VIRTIO_MMIO_DEVICE_FEATURES 0x010
// #define VIRTIO_MMIO_DRIVER_FEATURES 0x020

// #define VIRTIO_GPU_F_VIRGL          0x0001
// #define VIRTIO_GPU_CMD_GET_DISPLAY  0x0100
// #define VIRTIO_GPU_CMD_SET_SCANOUT  0x0101
// #define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0102
// #define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0103
// #define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0104
// #define VIRTIO_GPU_CMD_FLUSH        0x0105

// #define VIRTIO_MMIO_BASE            0x10001000  // Typically the base address for virtio devices on virt

// #define VIRTIO_BASE 0x10001000
// #define VIRTIO_MAGIC (VIRTIO_BASE + VIRTIO_MMIO_MAGIC_VALUE)
// #define VIRTIO_VERSION (VIRTIO_BASE + VIRTIO_MMIO_VERSION)
// #define VIRTIO_DEVICE_ID (VIRTIO_BASE + VIRTIO_MMIO_DEVICE_ID)
// #define VIRTIO_STATUS (VIRTIO_BASE + VIRTIO_MMIO_STATUS)
// #define VIRTIO_QUEUE_PFN (VIRTIO_BASE + VIRTIO_MMIO_QUEUE_PFN)

// #define FRAMEBUFFER_WIDTH 800
// #define FRAMEBUFFER_HEIGHT 600
// #define FRAMEBUFFER_BPP 32

#define VIRTIO_MMIO_STATUS 0x70

#define VIRTIO_STATUS_RESET 0x0
#define VIRTIO_STATUS_ACKNOWLEDGE   0x01
#define VIRTIO_STATUS_DRIVER       0x02
#define VIRTIO_STATUS_DRIVER_OK     0x04
#define VIRTIO_STATUS_FEATURES_OK   0x08
#define VIRTIO_STATUS_FAILED       0x80

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x100

#define PCI_BAR4 0x20
#define PCI_BAR5 0x24

uint64_t setup_gpu_bars(uint64_t base) {
    uart_puts("Setting up GPU BAR4 and BAR5...\n");

    uart_puts("Writing BAR4 and BAR5...\n");

    mmio_write(base + 0x20, 0xFFFFFFFF);

    uint64_t bar4 = mmio_read(base + PCI_BAR4);
    uint64_t bar5 = mmio_read(base + PCI_BAR5);

    if (bar4 == 0 || bar4 == 0xFFFFFFFF) {
        uart_puts("BAR4 size probing failed\n");
        return 0x0;
    }
    
    uint64_t size = ((uint64_t)(~((bar5 << 32) | (bar4 & ~0xF))) + 1);
    uart_puts("Calculated BAR size: ");
    uart_puthex(size);
    uart_putc('\n');

    uint64_t mmio_base = 0x10010000;
    mmio_write(base + 0x20, mmio_base & 0xFFFFFFFF);
    mmio_write(base + 0x24, (mmio_base >> 32) & 0xFFFFFFFF);

    // Step 5: Confirm the setup
    bar4 = mmio_read(base + PCI_BAR4);
    bar5 = mmio_read(base + PCI_BAR5);
    return ((uint64_t)bar5 << 32) | (bar4 & ~0xF);
}

void virtio_gpu_display_on(uint64_t base_addr) {
    // mmio_write(base_addr + 0x14, 1); // Set the ACKNOWLEDGE status bit
    // mmio_write(base_addr + 0x14, 2); // Set the DRIVER status bit

    // uint64_t features = mmio_read(base_addr + 0x4); // Read supported features
    // mmio_write(base_addr + 0x8, features); // Write selected features

    // mmio_write(base_addr + 0x14, 8); // Set the FEATURES_OK status bit
    // if (!(mmio_read(base_addr + 0x14) & 8)) {
    //     uart_puts("Features OK not set, device unusable\n");
    //     return;
    // }

    // Setting up the virtqueue (assuming queue 0 for simplicity)
    mmio_write(base_addr + 0x30, 0);   // queue_select
    mmio_write(base_addr + 0x38, 128); // queue_size
    mmio_write(base_addr + 0x3C, 1);   // queue_enable

    // Send display on command
    mmio_write(base_addr + 0x20, VIRTIO_GPU_CMD_GET_DISPLAY_INFO);

    mmio_write(base_addr + 0x14, 4); // Set the DRIVER_OK status bit

    uint64_t status = mmio_read(base_addr + 0x14);
    if (status & 4) {
        uart_puts("Display activated\n");
    } else {
        uart_puts("Display activation failed\n");
    }
}

void virtio_gpu_init(uint64_t base_addr) {
    mmio_write(base_addr + 0x14, 0); // Reset the device
    while (mmio_read(base_addr + 0x14) != 0);

    mmio_write(base_addr + 0x14, 1); // Acknowledge the device
    mmio_write(base_addr + 0x14, 2); // Driver

    mmio_write(base_addr + 0x0, 0); // Select feature bits 0-31
    uint64_t features = mmio_read(base_addr + 0x4); // Read features
    mmio_write(base_addr + 0x8, 0); // Select driver features 0-31
    mmio_write(base_addr + 0xC, features); // Write features

    mmio_write(base_addr + 0x14, 8); // Features OK
    if (!(mmio_read(base_addr + 0x14) & 8)) {
        uart_puts("Features OK not set, device unusable\n");
        return;
    }

    mmio_write(base_addr + 0x14, 4); // Driver OK
    uart_puts("GPU initialization complete\n");
}

void gpu_init() {

    uint64_t mmio_base;

    uint64_t address = find_pci_device(0x1AF4, 0x1050, &mmio_base);

    if (address > 0) {
        uart_puts("Virtio GPU detected at ");
        uart_puthex(address);
        
        uart_puts("Initializing GPU...\n");
        
        // pci_disable_device(address);
        mmio_base = setup_gpu_bars(address);
        pci_enable_device(address);

        if (mmio_base == 0) {
            uart_puts("Failed to read GPU MMIO base.\n");
            return;
        }
        uart_puts("MMIO base: ");
        uart_puthex(mmio_base);
        uart_putc('\n');

        virtio_gpu_init(mmio_base);

        virtio_gpu_display_on(mmio_base);
    }

    // if (mmio_read(VIRTIO_MAGIC) != 0x74726976 || mmio_read(VIRTIO_VERSION) != 2) return;

    // if (mmio_read(VIRTIO_DEVICE_ID) != 16) return;

    // mmio_write(VIRTIO_STATUS, 0x1); 

    // mmio_write(VIRTIO_MMIO_QUEUE_PFN, 0);

    // uint32_t features = mmio_read(VIRTIO_MMIO_DEVICE_FEATURES);
    // if (features & VIRTIO_GPU_F_VIRGL) {
    //     mmio_write(VIRTIO_MMIO_DRIVER_FEATURES, VIRTIO_GPU_F_VIRGL);
    // } else {
    //     mmio_write(VIRTIO_MMIO_DRIVER_FEATURES, 0);
    // }

    // mmio_write(VIRTIO_STATUS, 0x4);
    // mmio_write(VIRTIO_STATUS, 0x8);
}

// void gpu_clear(uint32_t color) {
//     volatile uint32_t *fb = (volatile uint32_t *)0x80000000;
//     for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++) {
//         for (int x = 0; x < FRAMEBUFFER_WIDTH; x++) {
//             fb[y * FRAMEBUFFER_WIDTH + x] = color;
//         }
//     }
// }

// void gpu_debug() {
//     uart_puts("Virtio GPU Debug:\n");

//     uint32_t magic = mmio_read(VIRTIO_BASE + 0x000);
//     uart_puts("Magic: ");
//     uart_puthex(magic);

//     uint32_t version = mmio_read(VIRTIO_BASE + 0x004);
//     uart_puts("Version: ");
//     uart_puthex(version);

//     uint32_t device_id = mmio_read(VIRTIO_BASE + 0x008);
//     uart_puts("Device ID: ");
//     uart_puthex(device_id);

//     uint32_t vendor_id = mmio_read(VIRTIO_BASE + 0x00c);
//     uart_puts("Vendor ID: ");
//     uart_puthex(vendor_id);

//     uint32_t status = mmio_read(VIRTIO_BASE + 0x070);
//     uart_puts("Status: ");
//     uart_puthex(status);

//     uint32_t queue_pfn = mmio_read(VIRTIO_BASE + 0x040);
//     uart_puts("Queue PFN: ");
//     uart_puthex(queue_pfn);

//     uint32_t features = mmio_read(VIRTIO_BASE + 0x010);
//     uart_puts("Device Features: ");
//     uart_puthex(features);
// }