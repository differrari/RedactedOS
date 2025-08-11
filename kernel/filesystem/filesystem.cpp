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

typedef struct {
    uint64_t file_id;
    size_t file_size;
    driver_module* mod;
} open_file_descriptors;

LinkedList<open_file_descriptors> *open_files;

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

FS_RESULT open_file(const char* path, file* descriptor){
    const char *search_path = path;
    driver_module *mod = get_module(&search_path);
    if (!mod) return FS_RESULT_NOTFOUND;
    
    // Initialize descriptor fields to safe values before calling driver
    descriptor->cursor = 0;
    descriptor->size = 0;
    descriptor->id = 0;
    
    FS_RESULT result = mod->open(search_path, descriptor);
    
    // Ensure cursor is still 0 after driver call
    if (descriptor->cursor != 0) {
        descriptor->cursor = 0;
    }
    
    if (!open_files)
        open_files = new LinkedList<open_file_descriptors>();
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
    
    // Find the file descriptor
    auto found = open_files->find([descriptor](open_file_descriptors kvp){
        return descriptor->id == kvp.file_id;
    });
    
    if (!found) {
        kprintf("[FS] File descriptor not found: %x", descriptor->id);
        return 0;
    }
    
    open_file_descriptors file = found->data;
    if (!file.mod) {
        kprintf("[FS] No filesystem module for file");
        return 0;
    }
    
    if (!file.mod->read) {
        kprintf("[FS] No read function in filesystem module");
        return 0;
    }
    
    // Fix corrupted cursor - reset to 0 for read from beginning
    if (descriptor->cursor > file.file_size) {
        kprintf("[FS] WARNING: Corrupted cursor %x, resetting to 0", descriptor->cursor);
        descriptor->cursor = 0;
    }
    
    // Calculate how much we can actually read
    size_t available = (descriptor->cursor >= file.file_size) ? 0 : (file.file_size - descriptor->cursor);
    size_t read_size = min(size, available);
    
    if (read_size == 0) {
        return 0;
    }
    
    // Call the driver once for the entire read
    size_t amount_read = file.mod->read(descriptor, buf, read_size, 0);
    
    return amount_read;
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
    if (!mod) return {0,0};
    return mod->readdir(search_path);
}