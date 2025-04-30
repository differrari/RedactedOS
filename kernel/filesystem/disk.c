#include "disk.h"
#include "dtb.h"
#include "kstring.h"
#include "ram_e.h"
#include "mmu.h"
#include "console/kio.h"
#include "pci.h"

static uint64_t disk_device_address;
static uint64_t disk_device_size;
static uint32_t disk_device_interrupt;

struct virtio_mmio_blk_regs {
    uint32_t magic;                 // 0x000
    uint32_t version;              // 0x004
    uint32_t device_id;            // 0x008
    uint32_t vendor_id;            // 0x00C
    uint32_t device_features;      // 0x010
    uint32_t device_features_sel;  // 0x014
    uint32_t reserved0[2];         // 0x018 - 0x01F
    uint32_t driver_features;      // 0x020
    uint32_t driver_features_sel;  // 0x024
    uint32_t reserved1[2];         // 0x028 - 0x02F
    uint32_t queue_sel;            // 0x030
    uint32_t queue_num_max;        // 0x034
    uint32_t queue_num;            // 0x038
    uint32_t queue_ready;          // 0x03C
    uint32_t queue_notify;         // 0x040
    uint32_t reserved2[3];         // 0x044 - 0x04F
    uint32_t interrupt_status;     // 0x050
    uint32_t interrupt_ack;        // 0x054
    uint32_t reserved3[2];         // 0x058 - 0x05F
    uint32_t status;               // 0x060
    uint32_t reserved4[3];         // 0x064 - 0x06F
    uint64_t queue_desc;           // 0x070
    uint64_t queue_driver;         // 0x078
    uint64_t queue_device;         // 0x080
    uint32_t config_generation;    // 0x088
    uint32_t reserved5[3];         // 0x08C - 0x097
    // device-specific config starts at 0x100
} __attribute__((packed));

#define VIRTIO_BLK_SUPPORTED_FEATURES \
    ((1 << 0) | (1 << 1) | (1 << 4))

int handle_virtio_node(const char *name, const char *propname, const void *prop, uint32_t len, dtb_match_t *match) {
    if (strcmp(propname, "reg") == 0 && len >= 16) {
        uint32_t *p = (uint32_t *)prop;
        match->reg_base = ((uint64_t)__builtin_bswap32(p[0]) << 32) | __builtin_bswap32(p[1]);
        match->reg_size = ((uint64_t)__builtin_bswap32(p[2]) << 32) | __builtin_bswap32(p[3]);
        uint8_t device_id = read32(match->reg_base + 0x008);
        match->found = device_id == 2;
        return 1;
    }
    else if (match->found && strcmp(propname, "interrupts") == 0 && len >= 4) {
        match->irq = __builtin_bswap32(*(uint32_t *)prop);
        return 1;
    }
    return 0;
}

int find_virtio_blk(uint64_t *out_base, uint64_t *out_size, uint32_t *out_irq) {
    dtb_match_t disk_info = {0};
    if (dtb_scan("virtio_mmio",handle_virtio_node, &disk_info)) {
        *out_base = disk_info.reg_base;
        *out_size = disk_info.reg_size;
        *out_irq = disk_info.irq;
        return 1;
    }
    return 0;
}

void find_disk(){
    uint64_t address = find_pci_device(0x1AF4, 0x1001);
    kprintf("Address %h",address);
    find_virtio_blk(&disk_device_address, &disk_device_size, &disk_device_interrupt);
}

bool init_disk() {
    volatile struct virtio_mmio_blk_regs *blk = (void *)disk_device_address;

    blk->status = 0;
    while (blk->status != 0);

    blk->status |= 1; // ACKNOWLEDGE
    blk->status |= 2; // DRIVER

    blk->device_features_sel = 0;
    uint32_t features = blk->device_features;
    kprintf("Raw features: %h", features);
    blk->driver_features_sel = 0;
    blk->driver_features = 0;//features & VIRTIO_BLK_SUPPORTED_FEATURES; // disable read-only
    kprintf("Accepted features: %h", blk->driver_features);

    blk->status |= 8; // FEATURES_OK
    if (!(blk->status & 0x8)) {
        kprintf("FEATURES_OK not accepted, device unusable");
        return false;
    }

    blk->queue_sel = 0;
    uint32_t qsize = blk->queue_num_max;
    blk->queue_num = qsize;

    blk->queue_desc = palloc(4096);
    blk->queue_driver = palloc(4096);
    blk->queue_device = palloc(4096);

    blk->queue_ready = 1;
    blk->status |= 4; // DRIVER_OK

    kprintf("Virtio disk initialized");
    return true;
}

uint64_t get_disk_address(){
    return disk_device_address;
}

uint64_t get_disk_size(){
    return disk_device_size;
}