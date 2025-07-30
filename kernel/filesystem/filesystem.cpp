#include "filesystem.h"
#include "fat32.hpp"
#include "mbr.h"
#include "fsdriver.hpp"
#include "std/std.hpp"
#include "dev/device_filesystem.hpp"

typedef struct mountkvp {
    const char* mount_point;
    FSDriver *driver;
} mountkvp;

LinkedList<mountkvp> *mountpoints = 0;

void mount(FSDriver *driver, const char *mount_point){
    if (!mountpoints) mountpoints = new LinkedList<mountkvp>();
    mountpoints->push_front((mountkvp){mount_point, driver});
}

bool init_boot_filesystem(){
    uint32_t f32_partition = mbr_find_partition(0xC);
    FAT32FS *fs_driver = new FAT32FS();
    mount(fs_driver, "/boot");
    return fs_driver->init(f32_partition);
}

bool init_dev_filesystem(){
    DeviceFS *fs_driver = new DeviceFS();
    mount(fs_driver,"/dev");
    return fs_driver->init(0);
}

FSDriver* get_fs(const char **full_path){
    auto lamdba = [&full_path](mountkvp kvp){
        int index = strstart(*full_path, kvp.mount_point, false);
        if (index == (int)strlen(kvp.mount_point,0)){
            *full_path += index;
            return true;
        }
        return false;
    };
    auto node = mountpoints->find(lamdba);
    if (!node) return NULL;
    return node->data.driver;
}

void* read_file(const char *path, size_t size){
    FSDriver *selected_driver = get_fs(&path);
    if (!selected_driver) return 0;
    return selected_driver->read_file(path, size);
}

string_list* list_directory_contents(const char *path){
    FSDriver *selected_driver = get_fs(&path);
    if (!selected_driver) return 0;
    return selected_driver->list_contents(path);
}