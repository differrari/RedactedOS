#include "console/kio.h"
#include "pci.h"
#include "memory/memory_access.h"
#include "memory/kalloc.h"
#include "virtio_pci.h"

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

#define VIRTIO_STATUS_RESET         0x0
#define VIRTIO_STATUS_ACKNOWLEDGE   0x1
#define VIRTIO_STATUS_DRIVER        0x2
#define VIRTIO_STATUS_DRIVER_OK     0x4
#define VIRTIO_STATUS_FEATURES_OK   0x8
#define VIRTIO_STATUS_FAILED        0x80

#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4
#define VIRTIO_PCI_CAP_PCI_CFG      5
#define VIRTIO_PCI_CAP_VENDOR_CFG   9

#define VIRTQ_DESC_F_NEXT 1

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

#define VIRTQ_DESC_F_NEXT 1
#define VIRTQ_DESC_F_WRITE 2

static bool virtio_verbose = false;

void virtio_enable_verbose(){
    virtio_verbose = true;
}

#define kprintfv(fmt, ...) \
    ({ \
        if (virtio_verbose){\
            uint64_t _args[] = { __VA_ARGS__ }; \
            kprintf_args((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
        }\
    })

void virtio_get_capabilities(virtio_device *dev, uint64_t pci_addr, uint64_t *mmio_start, uint64_t *mmio_size) {
    uint64_t offset = read32(pci_addr + 0x34);
    while (offset) {
        uint64_t cap_addr = pci_addr + offset;
        struct virtio_pci_cap* cap = (struct virtio_pci_cap*)(uintptr_t)cap_addr;
        uint64_t bar_addr = pci_get_bar_address(pci_addr, 0x10, cap->bar);
        uint64_t val = read32(bar_addr) & ~0xF;

        if (cap->cap_vndr == 0x9) {
            if (cap->cfg_type < VIRTIO_PCI_CAP_PCI_CFG && val == 0){
                kprintfv("[VIRTIO] Setting up bar");
                val = pci_setup_bar(pci_addr, cap->bar, mmio_start, mmio_size);
                kprintfv("[VIRTIO] Bar @ %h", val);
            }

            if (cap->cfg_type == VIRTIO_PCI_CAP_COMMON_CFG){
                kprintfv("[VIRTIO] Common CFG @ %h",val + cap->offset);
                dev->common_cfg = (struct virtio_pci_common_cfg*)(uintptr_t)(val + cap->offset);
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                kprintfv("[VIRTIO] Notify CFG @ %h",val + cap->offset);
                dev->notify_cfg = (uint8_t*)(uintptr_t)(val + cap->offset);
                dev->notify_off_multiplier = *(uint32_t*)(uintptr_t)(cap_addr + sizeof(struct virtio_pci_cap));
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG){
                kprintfv("[VIRTIO] Device CFG @ %h",val + cap->offset);
                dev->device_cfg = (uint8_t*)(uintptr_t)(val + cap->offset);
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_ISR_CFG){
                kprintfv("[VIRTIO] ISR CFG @ %h",val + cap->offset);
                dev->isr_cfg = (uint8_t*)(uintptr_t)(val + cap->offset);
            }
        }

        offset = cap->cap_next;
    }
}

bool virtio_init_device(virtio_device *dev) {

    struct virtio_pci_common_cfg* cfg = dev->common_cfg;

    cfg->device_status = 0;
    while (cfg->device_status != 0);

    cfg->device_status |= VIRTIO_STATUS_ACKNOWLEDGE;
    cfg->device_status |= VIRTIO_STATUS_DRIVER;

    cfg->device_feature_select = 0;
    uint32_t features = cfg->device_feature;

    cfg->driver_feature_select = 0;
    cfg->driver_feature = features;

    cfg->device_status |= VIRTIO_STATUS_FEATURES_OK;
    if (!(cfg->device_status & VIRTIO_STATUS_FEATURES_OK)){
        kprintf("Failed to negotiate features. Supported features %h",features);
        return false;
    }

    cfg->queue_select = 0;
    uint32_t size = cfg->queue_size;

    uint64_t base = palloc(4096);
    uint64_t avail = palloc(4096);
    uint64_t used = palloc(4096);

    kprintfv("[VIRTIO] Device base %h",base);
    kprintfv("[VIRTIO] Device avail %h",avail);
    kprintfv("[VIRTIO] Device used %h",used);

    cfg->queue_size = size;
    cfg->queue_desc = base;
    cfg->queue_driver = avail;
    cfg->queue_device = used;
    cfg->queue_enable = 1;

    cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;
    return true;
}

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

void virtio_send(virtio_device *dev, uint64_t desc, uint64_t avail, uint64_t used, uint64_t cmd, uint32_t cmd_len, uint64_t resp, uint32_t resp_len, uint8_t flags) {
    struct virtq_desc* d = (struct virtq_desc*)(uintptr_t)desc;
    struct virtq_avail* a = (struct virtq_avail*)(uintptr_t)avail;
    struct virtq_used* u = (struct virtq_used*)(uintptr_t)used;

    struct virtio_blk_req *req = (struct virtio_blk_req *)(uintptr_t)cmd;

    d[0].addr = cmd;
    d[0].len = cmd_len;
    d[0].flags = VIRTQ_DESC_F_NEXT;
    d[0].next = 1;
    
    d[1].addr = resp;
    d[1].len = resp_len;
    d[1].flags = VIRTQ_DESC_F_NEXT | flags;
    d[1].next = 2;
    
    uint8_t status = 0;
    d[2].addr = (uint64_t)&status;
    d[2].len = 1;
    d[2].flags = VIRTQ_DESC_F_WRITE;
    d[2].next = 0;

    a->ring[a->idx % 128] = 0;
    a->idx++;

    *(volatile uint16_t*)(uintptr_t)(dev->notify_cfg + dev->notify_off_multiplier * 0) = 0;

    uint16_t last_used_idx = u->idx;
    while (last_used_idx == u->idx);
    
    if (status != 0)
        kprintf("[VIRTIO OPERATION ERROR]: Wrong status %h",status);

}