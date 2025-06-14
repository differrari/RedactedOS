#include "disk.h"
#include "exfat.h"
#include "virtio_blk_pci.h"

static bool disk_enable_verbose;

void disk_verbose(){
    disk_enable_verbose = true;
    vblk_disk_verbose();
}

#define kprintfv(fmt, ...) \
    ({ \
        if (mmu_verbose){\
            uint64_t _args[] = { __VA_ARGS__ }; \
            kprintf_args_raw((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
        }\
    })

bool find_disk(){
    return vblk_find_disk();
}

bool disk_init(){
    return ef_init();
}

void disk_write(const void *buffer, uint32_t sector, uint32_t count){
    vblk_write(buffer, sector, count);
}

void disk_read(void *buffer, uint32_t sector, uint32_t count){
    vblk_read(buffer, sector, count);
}

void* read_file(const char *path){
    return ef_read_file(path);
}

string_list* list_directory_contents(const char *path){
    return ef_list_contents(path);
}