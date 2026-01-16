#pragma once 

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

#define VIRTIO_VENDOR 0x1AF4

#define VIRTIO_F_VERSION_1 32
#define VIRTIO_F_NOTIFICATION_DATA 38

typedef struct virtio_pci_common_cfg {
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
}__attribute__((packed)) virtio_pci_common_cfg;

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
}__attribute__((packed)) virtq_desc;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
}__attribute__((packed)) virtq_avail;

typedef struct {
    uint32_t id;
    uint32_t len;
}__attribute__((packed)) virtq_used_elem;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem ring[];
}__attribute__((packed)) virtq_used;

typedef struct virtio_device {
    struct virtio_pci_common_cfg* common_cfg;
    uint8_t* notify_cfg;
    uint8_t* device_cfg;
    uint8_t* isr_cfg;
    uint32_t notify_off_multiplier;
    void *memory_page;
    uint8_t* status_dma;
    uint64_t negotiated_features;
} virtio_device;

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
} virtio_buf;

#define VBUF(a,l,f) ((virtio_buf){.addr = (uint64_t)(a), .len = (uint32_t)(l), .flags = (uint16_t)(f)})

static inline void virtio_notify(virtio_device *dev){
    if(!dev) return;
    if(!dev->common_cfg) return;
    if(!dev->notify_cfg) return;

    uint16_t off=dev->common_cfg->queue_notify_off;
    uint32_t mul=dev->notify_off_multiplier;
    if(!mul) mul=1;
    
    uint16_t v=(dev->negotiated_features&(1ULL<<VIRTIO_F_NOTIFICATION_DATA))?dev->common_cfg->queue_notify_data:dev->common_cfg->queue_select;
    *(volatile uint16_t*)(dev->notify_cfg+(uint64_t)off*(uint64_t)mul)=v;
}

void virtio_set_feature_mask(uint64_t mask);
void virtio_enable_verbose();
void virtio_get_capabilities(virtio_device *dev, uint64_t pci_addr, uint64_t *mmio_start, uint64_t *mmio_size);
bool virtio_init_device(virtio_device *dev);
bool virtio_send_nd(virtio_device *dev, const virtio_buf *bufs, uint16_t n);
void virtio_add_buffer(virtio_device *dev, uint16_t index, uint64_t buf, uint32_t buf_len, bool host_to_dev);
uint32_t select_queue(virtio_device *dev, uint32_t index);

#ifdef __cplusplus
}
#endif