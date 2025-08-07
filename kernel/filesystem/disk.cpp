#include "disk.h"
#include "virtio_blk_pci.h"
#include "sdhci.hpp"
#include "hw/hw.h"
#include "console/kio.h"

static bool disk_enable_verbose;
SDHCI sdhci_driver; 

void disk_verbose(){
    disk_enable_verbose = true;
    vblk_disk_verbose();
    if (BOARD_TYPE == 2){
        sdhci_driver.enable_verbose();
    }
}

#define kprintfv(fmt, ...) \
    ({ \
        if (mmu_verbose){\
            kprintf(fmt, ##__VA_ARGS__); \
        }\
    })

bool init_disk_device(){
    if (BOARD_TYPE == 2)
        return sdhci_driver.init();
    else 
        return vblk_find_disk();
}

void disk_write(const void *buffer, uint32_t sector, uint32_t count){
    vblk_write(buffer, sector, count);
}

void disk_read(void *buffer, uint32_t sector, uint32_t count){
    if (BOARD_TYPE == 2)
        sdhci_driver.read(buffer, sector, count);
    else 
        vblk_read(buffer, sector, count);
}

driver_module disk_module = (driver_module){
    .name = "disk",
    .mount = "/disk",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = init_disk_device,
    .fini = 0,
    .open = 0,
    .read = 0,
    .write = 0,
    .seek = 0,
    .readdir = 0,
};