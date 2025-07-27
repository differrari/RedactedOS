#include "device_filesystem.hpp"
#include "console/kio.h"
#include "math/random.h"
#include "memory/page_allocator.h"

bool DeviceFS::init(uint32_t partition_sector){
    fs_page = alloc_page(0x1000, true, true, false);
    return true;
}

void* DeviceFS::read_file(char *path, size_t size){
    if (strcmp(path,"/zero",false) == 0){
        return create_buffer(size);
    }
    //TODO: random should return cryptographically secure random
    if (strcmp(path,"/random",false) == 0){
        void* buf = create_buffer(size);
        rng_fill_buf(&global_rng, buf, size);
        return buf;
    }
    return nullptr;
}

void* DeviceFS::create_buffer(size_t size){
    return allocate_in_page(fs_page, size, ALIGN_64B, true, true);
}

string_list* DeviceFS::list_contents(char *path){
    return nullptr;
}