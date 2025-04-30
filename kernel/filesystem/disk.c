#include "disk.h"
#include "dtb.h"
#include "kstring.h"
#include "ram_e.h"
#include "mmu.h"
#include "console/kio.h"
#include "pci.h"
#include "virtio/virtio_pci.h"

static uint64_t disk_device_address;
static uint64_t disk_device_size;

struct virtio_mmio_blk_regs {
    uint32_t magic;
    uint32_t version;
    uint32_t device_id;
    uint32_t vendor_id;
    uint32_t device_features;
    uint32_t device_features_sel;
    uint32_t reserved0[2];
    uint32_t driver_features;
    uint32_t driver_features_sel;
    uint32_t reserved1[2];
    uint32_t queue_sel;
    uint32_t queue_num_max;
    uint32_t queue_num;
    uint32_t queue_ready;
    uint32_t queue_notify;
    uint32_t reserved2[3];
    uint32_t interrupt_status;
    uint32_t interrupt_ack;
    uint32_t reserved3[2];
    uint32_t status;
    uint32_t reserved4[3];
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
    uint32_t config_generation; 
    uint32_t reserved5[3];
} __attribute__((packed));

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

static bool disk_enable_verbose;

void disk_verbose(){
    disk_enable_verbose = true;
    virtio_enable_verbose();
}

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

static virtio_device blk_dev;
static uint64_t blk_cmd, blk_data, blk_status;

bool find_disk(){
    uint64_t addr = find_pci_device(0x1AF4, 0x1001);
    if (!addr){ 
        kprintf("Disk device not found");
        return false;
    }

    virtio_get_capabilities(&blk_dev, addr, &disk_device_address, &disk_device_size);
    if (!virtio_init_device(&blk_dev)) {
        kprintf("Failed initialization");
        return false;
    }

    blk_cmd = palloc(4096);
    blk_data = palloc(4096);
    blk_status = palloc(1);

    return true;
}

void vblk_write(virtio_device *dev, const void *buffer, uint32_t sector, uint32_t count) {
    uint64_t cmd = palloc(sizeof(struct virtio_blk_req));
    uint64_t resp = palloc(1);
    uint64_t data = palloc(count * 512);

    memcpy((void *)(uintptr_t)data, buffer, count * 512);

    struct virtio_blk_req *req = (struct virtio_blk_req *)(uintptr_t)cmd;
    req->type = VIRTIO_BLK_T_OUT;
    req->reserved = 0;
    req->sector = sector;

    virtio_send(dev, dev->common_cfg->queue_desc, dev->common_cfg->queue_driver, dev->common_cfg->queue_device,
        cmd, sizeof(struct virtio_blk_req), data, count * 512, 0);
}

void vblk_read(virtio_device *dev, void *buffer, uint32_t sector, uint32_t count) {
    uint64_t cmd = talloc(sizeof(struct virtio_blk_req));
    uint64_t resp = talloc(count * 512);

    struct virtio_blk_req *req = (struct virtio_blk_req *)(uintptr_t)cmd;
    req->type = VIRTIO_BLK_T_IN;
    req->reserved = 0;
    req->sector = sector;

    virtio_send(dev, dev->common_cfg->queue_desc, dev->common_cfg->queue_driver, dev->common_cfg->queue_device, cmd, sizeof(struct virtio_blk_req), resp, count * 512, VIRTQ_DESC_F_WRITE);

    memcpy(buffer, (void *)(uintptr_t)resp, count * 512);

    temp_free((void *)cmd,sizeof(struct virtio_blk_req));
    temp_free((void *)resp,count * 512);
}

bool disk_test() {
    volatile struct virtio_blk_config *blk_cfg = (volatile struct virtio_blk_config *)((uint64_t)blk_dev.device_cfg);
    uint64_t num_sectors = blk_cfg->capacity;
    uint64_t bytes = num_sectors * 512;

    kprintf("Disk size: %h bytes (%h sectors)", bytes, num_sectors);

    const char *msg = "Written test";
    vblk_write(&blk_dev,msg,1,1);
    kprintf("Starting read test");
    char buf[512];
    vblk_read(&blk_dev, buf, 1, 1);
    kprintf("Read performed");
    kprintf("Read: %s",(uint64_t)buf);
}

uint64_t get_disk_address(){
    return disk_device_address;
}

uint64_t get_disk_size(){
    return disk_device_size;
}