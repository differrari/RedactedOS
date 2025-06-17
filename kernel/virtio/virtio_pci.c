#include "console/kio.h"
#include "pci.h"
#include "memory/memory_access.h"
#include "memory/page_allocator.h"
#include "virtio_pci.h"

//TODO: We're allocating way too much memory for each virtqueue element, we can reduce it down to the size of the structures, and use them more efficiently using rings instead of always writing the first command

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
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

uint32_t feature_mask;

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

void virtio_set_feature_mask(uint32_t mask){
    feature_mask = mask;
}

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
                kprintfv("[VIRTIO] Bar @ %x", val);
            }

            if (cap->cfg_type == VIRTIO_PCI_CAP_COMMON_CFG){
                kprintfv("[VIRTIO] Common CFG @ %x",val + cap->offset);
                dev->common_cfg = (struct virtio_pci_common_cfg*)(uintptr_t)(val + cap->offset);
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                kprintfv("[VIRTIO] Notify CFG @ %x",val + cap->offset);
                dev->notify_cfg = (uint8_t*)(uintptr_t)(val + cap->offset);
                dev->notify_off_multiplier = *(uint32_t*)(uintptr_t)(cap_addr + sizeof(struct virtio_pci_cap));
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG){
                kprintfv("[VIRTIO] Device CFG @ %x",val + cap->offset);
                dev->device_cfg = (uint8_t*)(uintptr_t)(val + cap->offset);
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_ISR_CFG){
                kprintfv("[VIRTIO] ISR CFG @ %x",val + cap->offset);
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

    kprintfv("Features %x",features);

    features &= feature_mask;

    kprintfv("Negotiated features %x",features);

    cfg->driver_feature_select = 0;
    cfg->driver_feature = features;

    cfg->device_status |= VIRTIO_STATUS_FEATURES_OK;
    if (!(cfg->device_status & VIRTIO_STATUS_FEATURES_OK)){
        kprintf("Failed to negotiate features. Supported features %x",features);
        return false;
    }

    dev->memory_page = alloc_page(0x1000, true, true, false);

    uint32_t queue_index = 0;
    uint32_t size;
    while ((size = select_queue(dev,queue_index))){
        uint64_t base = (uintptr_t)allocate_in_page(dev->memory_page, 16 * size, ALIGN_64B, true, true);
        uint64_t avail = (uintptr_t)allocate_in_page(dev->memory_page, 4 + (2 * size), ALIGN_64B, true, true);
        uint64_t used = (uintptr_t)allocate_in_page(dev->memory_page, sizeof(uint16_t) * (2 + size), ALIGN_64B, true, true);

        kprintfv("[VIRTIO QUEUE %i] Device base %x",queue_index,base);
        kprintfv("[VIRTIO QUEUE %i] Device avail %x",queue_index,avail);
        kprintfv("[VIRTIO QUEUE %i] Device used %x",queue_index,used);

        cfg->queue_desc = base;
        cfg->queue_driver = avail;
        cfg->queue_device = used;
        cfg->queue_enable = 1;
        queue_index++;
    }

    kprintfv("Device initialized %i virtqueues",queue_index);

    select_queue(dev,0);

    cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;
    return true;
}

uint32_t select_queue(virtio_device *dev, uint32_t index){
    dev->common_cfg->queue_select = index;
    asm volatile ("dsb sy" ::: "memory");
    return dev->common_cfg->queue_size;
}

bool virtio_send(virtio_device *dev, uint64_t desc, uint64_t avail, uint64_t used, uint64_t cmd, uint32_t cmd_len, uint64_t resp, uint32_t resp_len, uint8_t flags) {
    struct virtq_desc* d = (struct virtq_desc*)(uintptr_t)desc;
    struct virtq_avail* a = (struct virtq_avail*)(uintptr_t)avail;
    struct virtq_used* u = (struct virtq_used*)(uintptr_t)used;
    
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
    
    uint16_t last_used_idx = u->idx;
    a->ring[a->idx % 128] = 0;
    a->idx++;

    *(volatile uint16_t*)(uintptr_t)(dev->notify_cfg + dev->notify_off_multiplier * dev->common_cfg->queue_select) = 0;

    while (last_used_idx == u->idx);
    
    if (status != 0)
        kprintf("[VIRTIO OPERATION ERROR]: Wrong status %x",status);
    
    return status == 0;
}

bool virtio_send2(virtio_device *dev, uint64_t desc, uint64_t avail, uint64_t used, uint64_t cmd, uint32_t cmd_len, uint64_t resp, uint32_t resp_len, uint8_t flags) {

    struct virtq_desc* d = (struct virtq_desc*)(uintptr_t)desc;
    struct virtq_avail* a = (struct virtq_avail*)(uintptr_t)avail;
    struct virtq_used* u = (struct virtq_used*)(uintptr_t)used;
    uint16_t last_used_idx = u->idx;

    d[0].addr = cmd;
    d[0].len = cmd_len;
    d[0].flags = flags;
    d[0].next = 1;
    
    d[1].addr = resp;
    d[1].len = resp_len;
    d[1].flags = VIRTQ_DESC_F_WRITE;
    d[1].next = 0;

    a->ring[a->idx % 128] = 0;
    a->idx++;

    *(volatile uint16_t*)(uintptr_t)(dev->notify_cfg + dev->notify_off_multiplier * dev->common_cfg->queue_select) = 0;

    while (last_used_idx == u->idx);

    return true;
}

bool virtio_send3(virtio_device *dev, uint64_t cmd, uint32_t cmd_len) {

    struct virtq_desc* d = (struct virtq_desc*)(uintptr_t)dev->common_cfg->queue_desc;
    struct virtq_avail* a = (struct virtq_avail*)(uintptr_t)dev->common_cfg->queue_driver;
    struct virtq_used* u = (struct virtq_used*)(uintptr_t)dev->common_cfg->queue_device;
    uint16_t last_used_idx = u->idx;
    
    d[0].addr = cmd;
    d[0].len = cmd_len;
    d[0].flags = 0;
    d[0].next = 0;
    
    a->ring[a->idx % 128] = 0;

    a->idx++;

    *(volatile uint16_t*)(uintptr_t)(dev->notify_cfg + dev->notify_off_multiplier * dev->common_cfg->queue_select) = 0;

    while (last_used_idx == u->idx);

    return true;
}