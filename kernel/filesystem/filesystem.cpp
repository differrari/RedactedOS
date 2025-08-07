#include "filesystem.h"
#include "fat32.hpp"
#include "mbr.h"
#include "fsdriver.hpp"
#include "std/std.hpp"
#include "console/kio.h"
#include "dev/module_loader.h"
#include "memory/page_allocator.h"
#include "math/math.h"

FAT32FS *fs_driver;

bool boot_partition_init(){
    uint32_t f32_partition = mbr_find_partition(0xC);
    fs_driver = new FAT32FS();
    fs_driver->init(f32_partition);
    return true;
}

bool boot_partition_fini(){
    return false;
}

//TODO: find a way to make this more elegant
FS_RESULT boot_partition_open(const char *path, file *out_fd){
    //TODO: File descriptors are needed for F32
    return fs_driver->open_file(path, out_fd);
}

size_t boot_partition_read(file *fd, char *out_buf, size_t size, file_offset offset){
    //TODO: Need to pass a buffer and return a size instead, and use FD
    return fs_driver->read_file(fd, out_buf, size);
}

size_t boot_partition_write(file *fd, const char *buf, size_t size, file_offset offset){
    return 0;
}


file_offset boot_partition_seek(file *fd, file_offset offset){
    return 0;
}

sizedptr boot_partition_readdir(const char* path){
    //TODO: Need to pass a buffer and write to that, returning size
    return fs_driver->list_contents(path);
}

driver_module boot_fs_module = (driver_module){
    .name = "boot",
    .mount = "/boot",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = boot_partition_init,
    .fini = boot_partition_fini,
    .open = boot_partition_open,
    .read = boot_partition_read,
    .write = boot_partition_write,
    .seek = boot_partition_seek,
    .readdir = boot_partition_readdir,
};

bool init_boot_filesystem(){
    const char *path = "/disk";
    driver_module *disk_mod = get_module(&path);
    if (!disk_mod) return false;
    return load_module(&boot_fs_module);
}

void* read_file(const char *path, size_t size){
    const char *search_path = path;
    driver_module *mod = get_module(&search_path);
    if (!mod) return 0;
    file fd = {0,0};
    mod->open(search_path, &fd);
    void* pg = palloc(PAGE_SIZE, true, false, false);
    //TODO: TMP_BUF is not supposed to be used, you allocate your own memory 
    //TODO: There should be a separate open function, and keep track of which module handles which fd
    char *TMP_BUF = (char*)kalloc(pg, min(fd.size,size), ALIGN_64B, true, false);
    mod->read(&fd, TMP_BUF, min(fd.size,size), 0);
    return TMP_BUF;
}

sizedptr list_directory_contents(const char *path){
    const char *search_path = path;
    driver_module *mod = get_module(&search_path);
    if (!mod) return {0,0};
    return mod->readdir(search_path);
}