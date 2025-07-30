#pragma once

#include "types.h"
#include "filesystem/fsdriver.hpp"
#include "std/string.h"

class DeviceFS: public FSDriver {
public:
    bool init(uint32_t partition_sector) override;
    void* read_file(const char *path, size_t size) override;
    string_list* list_contents(const char *path) override;
private:
    void *fs_page = 0x0;
    void* create_buffer(size_t size);
};