#include "console/kio.h"
#include "pci.h"
#include "std/memory.h"
#include "std/memory_access.h"
#include "memory/page_allocator.h"
#include "virtio_pci.h"
#include "async.h"
#include "sysregs.h"

//TODO implement proper virtqueue handling w/ descriptor allocation, reuse and support for multiple in-flight requests using used.ring completions


#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4
#define VIRTIO_PCI_CAP_PCI_CFG      5
#define VIRTIO_PCI_CAP_VENDOR_CFG   9

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

bool virtio_verbose = false;
uint64_t feature_mask = 0;

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
    if (!dev) return;

    dev->common_cfg = 0;
    dev->notify_cfg = 0;
    dev->device_cfg = 0;
    dev->isr_cfg = 0;
    dev->notify_off_multiplier = 0;

    if (mmio_start) *mmio_start = 0;
    if (mmio_size) *mmio_size = 0;

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
                dev->common_cfg = (volatile struct virtio_pci_common_cfg*)PHYS_TO_VIRT((bar_base + cap->offset));
                kprintfv("[VIRTIO] Common CFG @ %llx",dev->common_cfg);
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                dev->notify_cfg = (volatile uint8_t*)PHYS_TO_VIRT((bar_base + cap->offset));
                kprintfv("[VIRTIO] Notify CFG @ %llx",dev->notify_cfg);
                dev->notify_off_multiplier = *(volatile uint32_t*)PHYS_TO_VIRT((cap_addr + sizeof(struct virtio_pci_cap)));
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG){
                dev->device_cfg = (volatile uint8_t*)PHYS_TO_VIRT((bar_base + cap->offset));
                kprintfv("[VIRTIO] Device CFG @ %llx",dev->device_cfg);
            } else if (cap->cfg_type == VIRTIO_PCI_CAP_ISR_CFG){
                dev->isr_cfg = (volatile uint8_t*)PHYS_TO_VIRT((bar_base + cap->offset));
                kprintfv("[VIRTIO] ISR CFG @ %llx",dev->isr_cfg);
            }
        }

        offset = cap->cap_next;
    }
}

bool virtio_init_device(virtio_device *dev) {
    if (!dev || !dev->common_cfg) return false;

    volatile struct virtio_pci_common_cfg* cfg = dev->common_cfg;

    memset(dev->queues, 0, sizeof(dev->queues));
    dev->num_queues = 0;
    dev->current_queue = 0;
    dev->negotiated_features = 0;
    uint32_t timeout = 2000;
    while (cfg->device_status != 0) {
        if (timeout == 0) return false;
        timeout--;
        delay(1);
    }

    cfg->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
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

    dev->num_queues = cfg->num_queues;
    if (dev->num_queues > VIRTIO_MAX_QUEUES) dev->num_queues = VIRTIO_MAX_QUEUES;

    for (uint16_t queue_index = 0; queue_index < dev->num_queues; queue_index++) {
        cfg->queue_select = queue_index;
        asm volatile ("dsb sy" ::: "memory");
        uint16_t size = cfg->queue_size;
        dev->queues[queue_index].size = size;
        dev->queues[queue_index].notify_off = cfg->queue_notify_off;
        dev->queues[queue_index].notify_data = cfg->queue_notify_data;

        if (!size) continue;
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

        memset(base, 0, desc_alloc);
        memset(avail, 0, avail_alloc);
        memset(used, 0, used_alloc);
        uint64_t desc_pa = VIRT_TO_PHYS((uint64_t)base);
        uint64_t driver_pa = VIRT_TO_PHYS((uint64_t)avail);
        uint64_t device_pa = VIRT_TO_PHYS((uint64_t)used);

        cfg->queue_size = size;
        cfg->queue_desc_lo = (uint32_t)desc_pa;
        cfg->queue_desc_hi = (uint32_t)(desc_pa >> 32);
        cfg->queue_driver_lo = (uint32_t)driver_pa;
        cfg->queue_driver_hi = (uint32_t)(driver_pa >> 32);
        cfg->queue_device_lo = (uint32_t)device_pa;
        cfg->queue_device_hi = (uint32_t)(device_pa >> 32);
        cfg->queue_enable = 1;

        dev->queues[queue_index].valid = true;
        dev->queues[queue_index].desc_pa = desc_pa;
        dev->queues[queue_index].driver_pa = driver_pa;
        dev->queues[queue_index].device_pa = device_pa;
        dev->queues[queue_index].desc = (volatile virtq_desc*)base;
        dev->queues[queue_index].driver = (volatile virtq_avail*)avail;
        dev->queues[queue_index].device = (volatile virtq_used*)used;
    }

    kprintfv("Device initialized %i virtqueues", dev->num_queues);

    for (uint16_t queue_index = 0; queue_index < dev->num_queues; queue_index++) {
        if (!dev->queues[queue_index].valid) continue;
        dev->current_queue = queue_index;
        cfg->queue_select = queue_index;
        asm volatile ("dsb sy" ::: "memory");
        return true;
    }

    return false;
}

uint32_t select_queue(virtio_device *dev, uint32_t index){
    if (!dev || !dev->common_cfg) return 0;
    dev->current_queue = (uint16_t)index;
    dev->common_cfg->queue_select = (uint16_t)index;
    asm volatile ("dsb sy" ::: "memory");
    if (index >= VIRTIO_MAX_QUEUES) return dev->common_cfg->queue_size;
    return dev->queues[index].size;
}

void virtio_notify(virtio_device *dev) {
    if (!dev || !dev->notify_cfg) return;
    uint16_t index = dev->current_queue;
    if (index >= VIRTIO_MAX_QUEUES) return;
    if (!dev->queues[index].valid) return;

    uint32_t mul = dev->notify_off_multiplier;
    if (!mul) mul = 1;

    uint16_t off = dev->queues[index].notify_off;
    uint16_t value = (dev->negotiated_features & (1ULL << VIRTIO_F_NOTIFICATION_DATA)) ? dev->queues[index].notify_data : index;

    *(volatile uint16_t*)((uintptr_t)dev->notify_cfg + (uint64_t)off * (uint64_t)mul) = value;
}

bool virtio_send_nd(virtio_device *dev, const virtio_buf *bufs, uint16_t n) {

    if (!dev || !bufs || !n) return false;

    if (dev->current_queue >= VIRTIO_MAX_QUEUES) return false;
    virtio_queue *queue = &dev->queues[dev->current_queue];
    if (!queue->valid || !queue->size || n > queue->size) return false;

    volatile virtq_desc* d = queue->desc;
    volatile virtq_avail* a = queue->driver;
    volatile virtq_used* u = queue->device;
    if (!d || !a || !u) return false;

    uint16_t qsz = queue->size;
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
    if (!dev) return;
    if (dev->current_queue >= VIRTIO_MAX_QUEUES) return;

    virtio_queue *queue = &dev->queues[dev->current_queue];
    if (!queue->valid || !queue->size) return;

    volatile virtq_desc* d = queue->desc;
    volatile virtq_avail* a = queue->driver;
    if (!d || !a) return;
    
    d[index].addr = VIRT_TO_PHYS(buf);
    d[index].len = buf_len;
    d[index].flags = host_to_dev ? 0 : VIRTQ_DESC_F_WRITE;
    d[index].next = 0;

    asm volatile ("dmb ishst" ::: "memory");
    a->ring[a->idx % queue->size] = index;
    asm volatile ("dmb ishst" ::: "memory");
    a->idx++;
    asm volatile ("dmb ishst" ::: "memory");
    virtio_notify(dev);
}