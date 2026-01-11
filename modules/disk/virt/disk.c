#include "filesystem/disk.h"
#include "console/kio.h"
#include "virtio/virtio_pci.h"
#include "pci.h"
#include "std/memory.h"
#include "sysregs.h"
#include "memory/page_allocator.h"

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

    return true;
}

void* disk_cmd;

void disk_write(const void *buffer, uint32_t sector, uint32_t count){
    if (!disk_cmd) disk_cmd = kalloc(blk_dev.memory_page, sizeof(virtio_blk_req), ALIGN_64B, MEM_PRIV_KERNEL);
    void* data = kalloc(blk_dev.memory_page, count * 512, ALIGN_64B, MEM_PRIV_KERNEL);

    memcpy(data, buffer, count * 512);

    virtio_blk_req *req = (virtio_blk_req *)disk_cmd;
    req->type = VIRTIO_BLK_T_OUT;
    req->reserved = 0;
    req->sector = sector;

    uint8_t status = 0;
    virtio_buf b[3] = {VBUF(disk_cmd, sizeof(virtio_blk_req), 0), VBUF(data, count * 512, 0), VBUF(&status, 1, VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&blk_dev, b, 3);
    kfree((void *)data,count * 512);
}

void disk_read(void *buffer, uint32_t sector, uint32_t count){
    if (!disk_cmd) disk_cmd = kalloc(blk_dev.memory_page, sizeof(virtio_blk_req), ALIGN_64B, MEM_PRIV_KERNEL);

    virtio_blk_req *req = (virtio_blk_req *)disk_cmd;
    req->type = VIRTIO_BLK_T_IN;
    req->reserved = 0;
    req->sector = sector;

    uint8_t status = 0;
    virtio_buf b[3] = {VBUF(disk_cmd, sizeof(virtio_blk_req), 0), VBUF(buffer, count * 512, VIRTQ_DESC_F_WRITE), VBUF(&status, 1, VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&blk_dev, b, 3);
}

system_module disk_module = (system_module){
    .name = "virtio_blk",
    .mount = "/disk",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = init_disk_device,
    .fini = 0,
    .open = 0,
    .read = 0,
    .write = 0,
    .close = 0,
    .sread = 0,
    .swrite = 0,
    .readdir = 0,
};