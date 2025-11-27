#include "console/kio.h"
#include "pci.h"
#include "std/memory_access.h"
#include "memory/page_allocator.h"
#include "virtio_pci.h"
#include "async.h"
#include "sysregs.h"

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

static uint64_t feature_mask = 0;

void virtio_enable_verbose(){
    virtio_verbose = true;
}

#define kprintfv(fmt, ...) \
    ({ \
        if (virtio_verbose){\
            kprintf(fmt, ##__VA_ARGS__); \
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

        uint64_t bar_reg = pci_get_bar_address(pci_addr, 0x10, cap->bar);
        uint32_t raw_lo = read32(bar_reg);
        uint64_t bar_base = (uint64_t)(raw_lo & ~0xFULL);
        if (raw_lo & 0x4) {
            uint32_t raw_hi = read32(bar_reg + 4);
            bar_base |= ((uint64_t)raw_hi) << 32;
        }

        if (cap->cap_vndr == 0x9) {
            if (cap->cfg_type < VIRTIO_PCI_CAP_PCI_CFG && bar_base == 0){
                kprintfv("[VIRTIO] Setting up bar");
                bar_base = pci_setup_bar(pci_addr, cap->bar, mmio_start, mmio_size);
                bar_base = VIRT_TO_PHYS(bar_base);
                kprintfv("[VIRTIO] Bar @ %llx", bar_base);
            }

            if (cap->cfg_type == VIRTIO_PCI_CAP_COMMON_CFG){
                dev->common_cfg = (struct virtio_pci_common_cfg*)PHYS_TO_VIRT((bar_base + cap->offset));
                kprintfv("[VIRTIO] Common CFG @ %llx",dev->common_cfg);
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                dev->notify_cfg = (uint8_t*)PHYS_TO_VIRT((bar_base + cap->offset));
                kprintfv("[VIRTIO] Notify CFG @ %llx",dev->notify_cfg);
                dev->notify_off_multiplier = *(uint32_t*)PHYS_TO_VIRT((cap_addr + sizeof(struct virtio_pci_cap)));
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG){
                dev->device_cfg = (uint8_t*)PHYS_TO_VIRT((bar_base + cap->offset));
                kprintfv("[VIRTIO] Device CFG @ %llx",dev->device_cfg);
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_ISR_CFG){
                dev->isr_cfg = (uint8_t*)PHYS_TO_VIRT((bar_base + cap->offset));
                kprintfv("[VIRTIO] ISR CFG @ %llx",dev->isr_cfg);
            }
        }

        offset = cap->cap_next;
    }
}

bool virtio_init_device(virtio_device *dev) {

    struct virtio_pci_common_cfg* cfg = dev->common_cfg;

    cfg->device_status = 0;
    if (!wait((uint32_t*)&cfg->device_status, 0, false, 2000)) return false;

    cfg->device_status |= VIRTIO_STATUS_ACKNOWLEDGE;
    cfg->device_status |= VIRTIO_STATUS_DRIVER;

    cfg->device_feature_select = 0;
    uint32_t f_lo = cfg->device_feature;
    cfg->device_feature_select = 1;
    uint32_t f_hi = cfg->device_feature;
    uint64_t features = ((uint64_t)f_hi << 32) | f_lo;

    kprintfv("Features %llx",(unsigned long long)features);

    uint64_t negotiated = (features & feature_mask);

    kprintfv("Negotiated features %llx",(unsigned long long)negotiated);

    cfg->driver_feature_select = 0;
    cfg->driver_feature = (uint32_t)(negotiated & 0xFFFFFFFFULL);
    cfg->driver_feature_select = 1;
    cfg->driver_feature = (uint32_t)(negotiated >> 32);

    cfg->device_status |= VIRTIO_STATUS_FEATURES_OK;
    if (!(cfg->device_status & VIRTIO_STATUS_FEATURES_OK)){
        kprintf("Failed to negotiate features. Supported features %llx",(unsigned long long)features);
        return false;
    }

    dev->memory_page = palloc(0x1000, MEM_PRIV_KERNEL, MEM_DEV | MEM_RW, false);
    if (!dev->memory_page) return false;

    dev->status_dma = (uint8_t*)kalloc(dev->memory_page, 64, ALIGN_4KB, MEM_PRIV_KERNEL);
    if (!dev->status_dma) return false;
    *dev->status_dma = 0;

    uint32_t queue_index = 0;
    uint32_t size;
    while ((size = select_queue(dev,queue_index))){
        uint64_t desc_sz  = 16ULL * size;
        uint64_t avail_sz = 4ULL + 2ULL * size;
        uint64_t used_sz  = 4ULL + 8ULL * size;
        uint64_t base = (uintptr_t)kalloc(dev->memory_page, desc_sz,  ALIGN_4KB, MEM_PRIV_KERNEL);
        uint64_t avail = (uintptr_t)kalloc(dev->memory_page, avail_sz, ALIGN_4KB, MEM_PRIV_KERNEL);
        uint64_t used = (uintptr_t)kalloc(dev->memory_page, used_sz,  ALIGN_4KB, MEM_PRIV_KERNEL);

        dev->common_cfg->queue_desc = VIRT_TO_PHYS(base);
        dev->common_cfg->queue_driver = VIRT_TO_PHYS(avail);
        dev->common_cfg->queue_device = VIRT_TO_PHYS(used);

        volatile virtq_avail* A = (volatile virtq_avail*)(uintptr_t)avail;
        A->flags = 0;
        A->idx = 0;

        volatile virtq_used* U = (volatile virtq_used*)(uintptr_t)used;
        U->flags = 0;
        U->idx = 0;

        dev->common_cfg->queue_enable = 1;
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

bool virtio_send_3d(virtio_device *dev, uint64_t cmd, uint32_t cmd_len, uint64_t resp, uint32_t resp_len, uint8_t flags) {
    volatile virtq_desc* d = PHYS_TO_VIRT_P((virtq_desc*)dev->common_cfg->queue_desc);
    volatile virtq_avail* a = PHYS_TO_VIRT_P((virtq_avail*)dev->common_cfg->queue_driver);
    volatile virtq_used* u = PHYS_TO_VIRT_P((virtq_used*)dev->common_cfg->queue_device);
    
    d[0].addr = VIRT_TO_PHYS(cmd);
    d[0].len = cmd_len;
    d[0].flags = VIRTQ_DESC_F_NEXT;
    d[0].next = 1;
    
    d[1].addr = VIRT_TO_PHYS(resp);
    d[1].len = resp_len;
    d[1].flags = VIRTQ_DESC_F_NEXT | flags;
    d[1].next = 2;
    
    *dev->status_dma = 0;
    d[2].addr = VIRT_TO_PHYS((uint64_t)dev->status_dma);
    d[2].len = 1;
    d[2].flags = VIRTQ_DESC_F_WRITE;
    d[2].next = 0;
    
    uint16_t last_used_idx = u->idx;
    a->ring[a->idx % dev->common_cfg->queue_size] = 0;
    a->idx++;

    *(volatile uint16_t*)(dev->notify_cfg + dev->notify_off_multiplier * dev->common_cfg->queue_select) = 0;

    while (last_used_idx == u->idx);//TODO: OPT

    uint8_t status = *dev->status_dma;
    if (status != 0)
        kprintf("[VIRTIO OPERATION ERROR]: Wrong status %x",status);
    
    return status == 0;
}

bool virtio_send_2d(virtio_device *dev, uint64_t cmd, uint32_t cmd_len, uint64_t resp, uint32_t resp_len, uint8_t flags) {

    volatile virtq_desc* d = PHYS_TO_VIRT_P((virtq_desc*)dev->common_cfg->queue_desc);
    volatile virtq_avail* a = PHYS_TO_VIRT_P((virtq_avail*)dev->common_cfg->queue_driver);
    volatile virtq_used* u = PHYS_TO_VIRT_P((virtq_used*)dev->common_cfg->queue_device);
    uint16_t last_used_idx = u->idx;

    d[0].addr = VIRT_TO_PHYS(cmd);
    d[0].len = cmd_len;
    d[0].flags = flags;
    d[0].next = 1;
    
    d[1].addr = VIRT_TO_PHYS(resp);
    d[1].len = resp_len;
    d[1].flags = VIRTQ_DESC_F_WRITE;
    d[1].next = 0;

    a->ring[a->idx % dev->common_cfg->queue_size] = 0;
    a->idx++;

    *(volatile uint16_t*)(dev->notify_cfg + dev->notify_off_multiplier * dev->common_cfg->queue_select) = 0;

    while (last_used_idx == u->idx);//TODO: OPT

    return true;
}

bool virtio_send_1d(virtio_device *dev, uint64_t cmd, uint32_t cmd_len) {

    volatile virtq_desc* d = PHYS_TO_VIRT_P((virtq_desc*)dev->common_cfg->queue_desc);
    volatile virtq_avail* a = PHYS_TO_VIRT_P((virtq_avail*)dev->common_cfg->queue_driver);
    volatile virtq_used* u = PHYS_TO_VIRT_P((virtq_used*)dev->common_cfg->queue_device);
    uint16_t last_used_idx = u->idx;
    
    d[0].addr = VIRT_TO_PHYS(cmd);
    d[0].len = cmd_len;
    d[0].flags = 0;
    d[0].next = 0;
    
    a->ring[a->idx % dev->common_cfg->queue_size] = 0;

    a->idx++;

    *(volatile uint16_t*)(dev->notify_cfg + dev->notify_off_multiplier * dev->common_cfg->queue_select) = 0;

    while (last_used_idx == u->idx);//TODO: OPT

    return true;
}

void virtio_add_buffer(virtio_device *dev, uint16_t index, uint64_t buf, uint32_t buf_len, bool host_to_dev) {

    volatile virtq_desc* d = PHYS_TO_VIRT_P((virtq_desc*)dev->common_cfg->queue_desc);
    volatile virtq_avail* a = PHYS_TO_VIRT_P((virtq_avail*)dev->common_cfg->queue_driver);
    
    d[index].addr = VIRT_TO_PHYS(buf);
    d[index].len = buf_len;
    d[index].flags = host_to_dev ? 0 : VIRTQ_DESC_F_WRITE;
    d[index].next = 0;
    
    a->ring[a->idx % dev->common_cfg->queue_size] = index;
    a->idx++;

    *(volatile uint16_t*)(dev->notify_cfg + dev->notify_off_multiplier * dev->common_cfg->queue_select) = 0;
}