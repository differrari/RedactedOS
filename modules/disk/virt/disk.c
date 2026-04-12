#include "filesystem/disk.h"
#include "console/kio.h"
#include "virtio/virtio_pci.h"
#include "pci.h"
#include "std/memory.h"
#include "sysregs.h"
#include "memory/page_allocator.h"
#include "exceptions/irq.h"

#define VIRTIO_BLK_T_IN   0
#define VIRTIO_BLK_T_OUT  1

typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed)) virtio_blk_req;

#define VIRTIO_BLK_SUPPORTED_FEATURES \
    ((1 << 0) | (1 << 1) | (1 << 4))


static bool blk_disk_enable_verbose;
static virtio_device blk_dev;
static virtio_blk_req *disk_cmd;
static uint8_t *disk_status;

#define VIRTIO_BLK_ID 0x1001

void disk_verbose(){
    blk_disk_enable_verbose = true;
    virtio_enable_verbose();
}

#define kprintfv(fmt, ...) \
    ({ \
        if (blk_disk_enable_verbose){\
            kprintf(fmt, ##__VA_ARGS__); \
        }\
    })

bool init_disk_device(){
    kprint("Initializing disk");
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

    disk_cmd = (virtio_blk_req*)kalloc(blk_dev.memory_page, sizeof(virtio_blk_req), ALIGN_64B, MEM_PRIV_KERNEL);
    disk_status = (uint8_t*)kalloc(blk_dev.memory_page, 64, ALIGN_64B, MEM_PRIV_KERNEL);
    if (!disk_cmd || !disk_status) {
        if (disk_cmd) kfree(disk_cmd, sizeof(virtio_blk_req));
        if (disk_status) kfree(disk_status, 64);
        kprintf("failed disk DMA buffer");
        return false;
    }


    blk_dev.common_cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;
    return true;
}

void disk_write(const void *buffer, uint32_t sector, uint32_t count){
    if (!disk_cmd || !disk_status || !count) return;
    irq_flags_t irq = irq_save_disable();
    
    void* data = kalloc(blk_dev.memory_page, count * 512, ALIGN_64B, MEM_PRIV_KERNEL);
    if (!data) {
        irq_restore(irq);
        return;
    }

    memcpy(data, buffer, count * 512);

    disk_cmd->type = VIRTIO_BLK_T_OUT;
    disk_cmd->reserved = 0;
    disk_cmd->sector = sector;

    disk_status[0] = 0;
    virtio_buf b[3] = {VBUF(disk_cmd, sizeof(virtio_blk_req), 0), VBUF(data, count * 512, 0), VBUF(disk_status, 1, VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&blk_dev, b, 3);
    kfree((void *)data,count * 512);
    irq_restore(irq);
}

void disk_read(void *buffer, uint32_t sector, uint32_t count){
    if (!disk_cmd || !disk_status || !count) return;

    irq_flags_t irq = irq_save_disable();
    disk_cmd->type = VIRTIO_BLK_T_IN;
    disk_cmd->reserved = 0;
    disk_cmd->sector = sector;

    disk_status[0] = 0;
    virtio_buf b[3] = {VBUF(disk_cmd, sizeof(virtio_blk_req), 0), VBUF(buffer, count * 512, VIRTQ_DESC_F_WRITE), VBUF(disk_status, 1, VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&blk_dev, b, 3);
    irq_restore(irq);
}

system_module disk_module = (system_module){
    .name = "virtio_blk",
    .mount = "disk",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = init_disk_device,
    .fini = 0,
    .open = 0,
    .read = 0,
    .write = 0,
    .close = 0,
    .readdir = 0,
};