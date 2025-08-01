#include "filesystem.h"
#include "fat32.hpp"
#include "mbr.h"
#include "fsdriver.hpp"
#include "std/std.hpp"
#include "console/kio.h"
#include "dev/module_loader.h"
#include "memory/page_allocator.h"

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

static driver_module boot_fs_module = (driver_module){
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
    return load_module(boot_fs_module);
}

void* read_file(const char *path, size_t size){
    driver_module *mod = &boot_fs_module;//get_module(&path);
    if (!mod) return 0;
    file fd = {0,0};
    mod->open(path, &fd);
    void* pg = palloc(PAGE_SIZE, true, false, false);
    char *TMP_BUF = (char*)kalloc(pg, fd.size, ALIGN_64B, true, false);
    mod->read(&fd, TMP_BUF, fd.size, 0);
    return TMP_BUF;
}

sizedptr list_directory_contents(const char *path){
    const char* path2 = "/boot/redos/userland";
    driver_module *mod = &boot_fs_module;//get_module(&path2);
    if (!mod) return {0,0};
    return mod->readdir(path);
}