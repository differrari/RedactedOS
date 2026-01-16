#include "console/kio.h"
#include "pci.h"
#include "std/memory_access.h"
#include "memory/page_allocator.h"
#include "virtio_pci.h"
#include "async.h"
#include "sysregs.h"

//TODO implement proper virtqueue handling w/ descriptor allocation, reuse and support for multiple in-flight requests using used.ring completions

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

void virtio_set_feature_mask(uint64_t mask){
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

    dev->negotiated_features = negotiated;

    cfg->device_status |= VIRTIO_STATUS_FEATURES_OK;
    if (!(cfg->device_status & VIRTIO_STATUS_FEATURES_OK)){
        kprintf("Failed to negotiate features. Supported features %llx",(unsigned long long)features);
        return false;
    }

    dev->memory_page = palloc(0x10000, MEM_PRIV_KERNEL, MEM_DEV | MEM_RW, false);
    if (!dev->memory_page) return false;

    dev->status_dma = (uint8_t*)kalloc(dev->memory_page, 64, ALIGN_4KB, MEM_PRIV_KERNEL);
    if (!dev->status_dma) return false;
    *dev->status_dma = 0;

    uint32_t queue_index = 0;
    uint32_t size;
    while ((size = select_queue(dev,queue_index))){
        uint64_t desc_sz  = 16ULL * size;
        uint64_t avail_sz = 6ULL + 2ULL * size;
        uint64_t used_sz = 6ULL + 8ULL * size;

        uint64_t desc_alloc = (desc_sz + (uint64_t)(PAGE_SIZE - 1)) & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t avail_alloc = (avail_sz + (uint64_t)(PAGE_SIZE - 1)) & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t used_alloc = (used_sz + (uint64_t)(PAGE_SIZE - 1)) & ~(uint64_t)(PAGE_SIZE - 1);

        void* base = palloc(desc_alloc, MEM_PRIV_KERNEL, MEM_DEV | MEM_RW, true);
        void* avail = palloc(avail_alloc, MEM_PRIV_KERNEL, MEM_DEV | MEM_RW, true);
        void* used = palloc(used_alloc, MEM_PRIV_KERNEL, MEM_DEV | MEM_RW, true);
        if (!base || !avail || !used) return false;

        dev->common_cfg->queue_desc = VIRT_TO_PHYS((uint64_t)base);
        dev->common_cfg->queue_driver = VIRT_TO_PHYS((uint64_t)avail);
        dev->common_cfg->queue_device = VIRT_TO_PHYS((uint64_t)used);

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

bool virtio_send_nd(virtio_device *dev, const virtio_buf *bufs, uint16_t n) {

    if (!dev || !bufs || !n) return false;

    uint16_t qsz = dev->common_cfg->queue_size;
    if (!qsz || n > qsz) return false;

    volatile virtq_desc* d = PHYS_TO_VIRT_P((virtq_desc*)dev->common_cfg->queue_desc);
    volatile virtq_avail* a = PHYS_TO_VIRT_P((virtq_avail*)dev->common_cfg->queue_driver);
    volatile virtq_used* u = PHYS_TO_VIRT_P((virtq_used*)dev->common_cfg->queue_device);
    uint16_t last_used_idx = u->idx;

    for (uint16_t i = 0; i < n; ++i) {
        if (!bufs[i].addr || !bufs[i].len) return false;
        d[i].addr = VIRT_TO_PHYS(bufs[i].addr);
        d[i].len = bufs[i].len;
        d[i].flags = bufs[i].flags;
        if (i + 1 < n) {
            d[i].flags |= VIRTQ_DESC_F_NEXT;
            d[i].next = (uint16_t)(i + 1);
        } else {
            d[i].next = 0;
        }
    }

    asm volatile ("dmb ishst" ::: "memory");

    a->ring[a->idx % qsz] = 0;
    asm volatile ("dmb ishst" ::: "memory");
    a->idx++;
    asm volatile ("dmb ishst" ::: "memory");
    virtio_notify(dev);

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

    asm volatile ("dmb ishst" ::: "memory");
    a->ring[a->idx % dev->common_cfg->queue_size] = index;
    asm volatile ("dmb ishst" ::: "memory");
    a->idx++;
    asm volatile ("dmb ishst" ::: "memory");
    virtio_notify(dev);
}