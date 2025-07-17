#include "disk.h"
#include "exfat.hpp"
#include "fat32.hpp"
#include "virtio_blk_pci.h"
#include "sdhci.hpp"
#include "hw/hw.h"
#include "console/kio.h"
#include "mbr.h"

static bool disk_enable_verbose;
SDHCI sdhci_driver; 
FAT32FS *fs_driver;

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
            uint64_t _args[] = { __VA_ARGS__ }; \
            kprintf_args_raw((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
        }\
    })


bool find_disk(){
    if (BOARD_TYPE == 2)
        return sdhci_driver.init();
    else 
        return vblk_find_disk();
}

bool disk_init(){
    uint32_t f32_partition = mbr_find_partition(0xC);
    fs_driver = new FAT32FS();
    return fs_driver->init(f32_partition);
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

void* read_file(char *path){
    return fs_driver->read_file(path);
}

string_list* list_directory_contents(char *path){
    return fs_driver->list_contents(path);
}