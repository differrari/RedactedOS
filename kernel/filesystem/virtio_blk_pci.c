#include "disk.h"
#include "std/string.h"
#include "memory/memory_access.h"
#include "memory/page_allocator.h"
#include "console/kio.h"
#include "pci.h"
#include "virtio/virtio_pci.h"
#include "std/memfunctions.h"
#include "virtio_blk_pci.h"

#define VIRTIO_BLK_T_IN   0
#define VIRTIO_BLK_T_OUT  1

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

struct virtio_blk_config {
    uint64_t capacity;//In number of sectors
    uint32_t size_max;
    uint32_t seg_max;
    struct {
        uint16_t cylinders;
        uint8_t heads;
        uint8_t sectors;
    } geometry;
    uint32_t blk_size;
} __attribute__((packed));

#define VIRTIO_BLK_SUPPORTED_FEATURES \
    ((1 << 0) | (1 << 1) | (1 << 4))

static bool blk_disk_enable_verbose;

void vblk_disk_verbose(){
    blk_disk_enable_verbose = true;
    virtio_enable_verbose();
}

#define kprintfv(fmt, ...) \
    ({ \
        if (blk_disk_enable_verbose){\
            kprintf(fmt, ##__VA_ARGS__); \
        }\
    })

static virtio_device blk_dev;

bool vblk_find_disk(){
    uint64_t addr = find_pci_device(VIRTIO_VENDOR, VIRTIO_BLK_ID);
    if (!addr){ 
        kprintf("Disk device not found");
        return false;
    }

    pci_enable_device(addr);

    uint64_t disk_device_address, disk_device_size;

    virtio_get_capabilities(&blk_dev, addr, &disk_device_address, &disk_device_size);
    pci_register(disk_device_address, disk_device_size);
    if (!virtio_init_device(&blk_dev)) {
        kprintf("Failed disk initialization");
        return false;
    }

    return true;
}

void vblk_write(const void *buffer, uint32_t sector, uint32_t count) {
    void* cmd = kalloc(blk_dev.memory_page, sizeof(struct virtio_blk_req), ALIGN_64B, true, true);
    void* data = kalloc(blk_dev.memory_page, count * 512, ALIGN_64B, true, true);

    memcpy(data, buffer, count * 512);

    struct virtio_blk_req *req = (struct virtio_blk_req *)(uintptr_t)cmd;
    req->type = VIRTIO_BLK_T_OUT;
    req->reserved = 0;
    req->sector = sector;

    virtio_send(&blk_dev, blk_dev.common_cfg->queue_desc, blk_dev.common_cfg->queue_driver, blk_dev.common_cfg->queue_device,
        (uintptr_t)cmd, sizeof(struct virtio_blk_req), (uintptr_t)data, count * 512, 0);

    kfree((void *)cmd,sizeof(struct virtio_blk_req));
    kfree((void *)data,count * 512);
}

void vblk_read(void *buffer, uint32_t sector, uint32_t count) {
    void* cmd = kalloc(blk_dev.memory_page, sizeof(struct virtio_blk_req), ALIGN_64B, true, true);
    void* data = kalloc(blk_dev.memory_page, count * 512, ALIGN_64B, true, true);

    struct virtio_blk_req *req = (struct virtio_blk_req *)cmd;
    req->type = VIRTIO_BLK_T_IN;
    req->reserved = 0;
    req->sector = sector;

    virtio_send(&blk_dev, blk_dev.common_cfg->queue_desc, blk_dev.common_cfg->queue_driver, blk_dev.common_cfg->queue_device, (uintptr_t)cmd, sizeof(struct virtio_blk_req), (uintptr_t)data, count * 512, VIRTQ_DESC_F_WRITE);

    memcpy(buffer, (void *)(uintptr_t)data, count * 512);

    kfree((void *)cmd,sizeof(struct virtio_blk_req));
    kfree((void *)data,count * 512);
}