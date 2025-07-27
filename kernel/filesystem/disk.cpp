#include "disk.h"
#include "exfat.hpp"
#include "fat32.hpp"
#include "virtio_blk_pci.h"
#include "sdhci.hpp"
#include "hw/hw.h"
#include "console/kio.h"
#include "mbr.h"
#include "fsdriver.hpp"
#include "std/std.hpp"

static bool disk_enable_verbose;
SDHCI sdhci_driver; 
FAT32FS *fs_driver;

typedef struct mountkvp {
    char* mount_point;
    FSDriver *driver;
} mountkvp;

LinkedList<mountkvp> *mountpoints;

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

bool init_disk_device(){
    if (BOARD_TYPE == 2)
        return sdhci_driver.init();
    else 
        return vblk_find_disk();
}

void mount(FSDriver *driver, char *mount_point){
    if (!mountpoints) mountpoints = new LinkedList<mountkvp>();
    mountpoints->push_front((mountkvp){mount_point, driver});
}

bool init_boot_filesystem(){
    uint32_t f32_partition = mbr_find_partition(0xC);
    fs_driver = new FAT32FS();
    mount(fs_driver, "/boot");
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

FSDriver* get_fs(char **full_path){
    auto lamdba = [&full_path](mountkvp kvp){
        int index = strstart(*full_path, kvp.mount_point, false);
        kprintf("Comparing %s and %s = %i",(uintptr_t)*full_path,(uintptr_t)kvp.mount_point, index);
        if (index > 0){
            *full_path += index;
            return true;
        }
        return false;
    };
    auto node = mountpoints->find(lamdba);
    if (!node) return NULL;
    return node->data.driver;
}

void* read_file(char *path){
    kprintf("Searching for file at path %s",(uintptr_t)path);
    FSDriver *selected_driver = get_fs(&path);
    kprintf("Path %s will be handled by %x",(uintptr_t)path,(uintptr_t)selected_driver);
    if (!selected_driver) return 0;
    return selected_driver->read_file(path);
}

string_list* list_directory_contents(char *path){
    kprintf("Searching for file at path %s",(uintptr_t)path);
    FSDriver *selected_driver = get_fs(&path);
    kprintf("Path %s will be handled by %x",(uintptr_t)path,(uintptr_t)selected_driver);
    if (!selected_driver) return 0;
    return selected_driver->list_contents(path);
}