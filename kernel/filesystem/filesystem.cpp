#include "filesystem.h"
#include "exfat.hpp"
#include "fat32.hpp"
#include "mbr.h"
#include "fsdriver.hpp"
#include "std/std.hpp"
#include "console/kio.h"

FAT32FS *fs_driver;

typedef struct mountkvp {
    char* mount_point;
    FSDriver *driver;
} mountkvp;

LinkedList<mountkvp> *mountpoints = 0;

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
    FSDriver *selected_driver = get_fs(&path);
    if (!selected_driver) return 0;
    return selected_driver->read_file(path);
}

string_list* list_directory_contents(char *path){
    FSDriver *selected_driver = get_fs(&path);
    if (!selected_driver) return 0;
    return selected_driver->list_contents(path);
}