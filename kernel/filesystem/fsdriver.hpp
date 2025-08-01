#pragma once

#include "std/string.h"
#include "types.h"
#include "dev/driver_base.h"

class FSDriver {
public:
    virtual bool init(uint32_t partition_sector) = 0;
    virtual FS_RESULT open_file(const char* path, file* descriptor) = 0;
    virtual size_t read_file(file *descriptor, void* buf, size_t size) = 0;
    virtual sizedptr list_contents(const char *path) = 0;
};