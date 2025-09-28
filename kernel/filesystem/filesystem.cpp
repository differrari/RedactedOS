#include "filesystem.h"
#include "fat32.hpp"
#include "mbr.h"
#include "fsdriver.hpp"
#include "std/std.h"
#include "console/kio.h"
#include "dev/module_loader.h"
#include "memory/page_allocator.h"
#include "math/math.h"
#include "virtio_9p_pci.hpp"

FAT32FS *fs_driver;

typedef struct {
    uint64_t file_id;
    size_t file_size;
    driver_module* mod;
} open_file_descriptors;

LinkedList<open_file_descriptors> *open_files;

bool boot_partition_init(){
    uint32_t f32_partition = mbr_find_partition(0xC);
    fs_driver = new FAT32FS();
    return fs_driver->init(f32_partition);
}

bool boot_partition_fini(){
    return false;
}

//TODO: find a way to make this more elegant
FS_RESULT boot_partition_open(const char *path, file *out_fd){
    return fs_driver->open_file(path, out_fd);
}

size_t boot_partition_read(file *fd, char *out_buf, size_t size, file_offset offset){
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

Virtio9PDriver *p9Driver;

bool shared_init(){
    p9Driver = new Virtio9PDriver();
    return p9Driver->init(0);
}

bool shared_fini(){
    return false;
}

FS_RESULT shared_open(const char *path, file *out_fd){
    return p9Driver->open_file(path, out_fd);
}

size_t shared_read(file *fd, char *out_buf, size_t size, file_offset offset){
    return p9Driver->read_file(fd, out_buf, size);
}

size_t shared_write(file *fd, const char *buf, size_t size, file_offset offset){
    return 0;
}

file_offset shared_seek(file *fd, file_offset offset){
    return 0;
}

sizedptr shared_readdir(const char* path){
    //TODO: Need to pass a buffer and write to that, returning size
    return p9Driver->list_contents(path);
}

driver_module p9_fs_module = (driver_module){
    .name = "9PFS",
    .mount = "/shared",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = shared_init,
    .fini = shared_fini,
    .open = shared_open,
    .read = shared_read,
    .write = shared_write,
    .seek = shared_seek,
    .readdir = shared_readdir,
};

bool init_filesystem(){
    const char *path = "/disk";
    driver_module *disk_mod = get_module(&path);
    if (!disk_mod) return false;
    return load_module(&boot_fs_module) && load_module(&p9_fs_module);
}

void* page;

FS_RESULT open_file(const char* path, file* descriptor){
    const char *search_path = path;
    driver_module *mod = get_module(&search_path);
    if (!mod) return FS_RESULT_NOTFOUND;
    FS_RESULT result = mod->open(search_path, descriptor);
    if (!open_files){
        page = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
        LinkedList<open_file_descriptors> *ptr = (LinkedList<open_file_descriptors>*)kalloc(page, sizeof(LinkedList<open_file_descriptors>), ALIGN_64B, MEM_PRIV_KERNEL);
        open_files = new (ptr) LinkedList<open_file_descriptors>();
        open_files->set_allocator(
        [](size_t size) -> void* {
            return kalloc(page, size, ALIGN_64B, MEM_PRIV_KERNEL);
        },
        [](void* ptr, size_t size) {
            kfree(ptr, size);
        }
    );
    }
    open_files->push_front({
        .file_id = descriptor->id,
        .file_size = descriptor->size,
        .mod = mod
    });
    return result;
}

size_t read_file(file *descriptor, char* buf, size_t size){
    if (!open_files){
        kprintf("[FS] No open files");
        return 0;
    }
    open_file_descriptors file = open_files->find([descriptor](open_file_descriptors kvp){
        return descriptor->id == kvp.file_id;
    })->data;
    if (!file.mod) return 0;
    size_t amount_read = file.mod->read(descriptor, buf, size, 0);
    descriptor->cursor += amount_read;
    return amount_read;
}

void close_file(file *descriptor){
    
}

size_t write_file(file *descriptor, const char* buf, size_t size){
    if (!open_files) return 0;
    open_file_descriptors file = open_files->find([descriptor](open_file_descriptors kvp){
        return descriptor->id == kvp.file_id;
    })->data;
    if (!file.mod) return 0;
    return file.mod->write(descriptor, buf, size, 0);
}

sizedptr list_directory_contents(const char *path){
    const char *search_path = path;
    driver_module *mod = get_module(&search_path);
    if (!mod){
        kprintf("No module for path");
        return {0,0};
    }
    return mod->readdir(search_path);
}