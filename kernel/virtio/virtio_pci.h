#pragma once 

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

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

typedef struct virtio_device {
    struct virtio_pci_common_cfg* common_cfg;
    uint8_t* notify_cfg;
    uint8_t* device_cfg;
    uint8_t* isr_cfg;
    uint32_t notify_off_multiplier;
    void *memory_page;
} virtio_device;

void virtio_set_feature_mask(uint32_t mask);
void virtio_enable_verbose();
void virtio_get_capabilities(virtio_device *dev, uint64_t pci_addr, uint64_t *mmio_start, uint64_t *mmio_size);
bool virtio_init_device(virtio_device *dev);
bool virtio_send(virtio_device *dev, uint64_t desc, uint64_t avail, uint64_t used, uint64_t cmd, uint32_t cmd_len, uint64_t resp, uint32_t resp_len, uint8_t flags);
bool virtio_send2(virtio_device *dev, uint64_t desc, uint64_t avail, uint64_t used, uint64_t cmd, uint32_t cmd_len, uint64_t resp, uint32_t resp_len, uint8_t flags);
bool virtio_send3(virtio_device *dev, uint64_t cmd, uint32_t cmd_len);
uint32_t select_queue(virtio_device *dev, uint32_t index);

#ifdef __cplusplus
}
#endif