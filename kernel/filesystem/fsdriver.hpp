#pragma once

#include "std/string.h"
#include "types.h"
#include "dev/driver_base.h"

class FSDriver {
public:
    virtual bool init(uint32_t partition_sector) = 0;
    virtual FS_RESULT open_file(const char* path, file* descriptor) = 0;
    virtual size_t read_file(file *descriptor, void* buf, size_t size) = 0;
    virtual size_t write_file(file *descriptor, const char* buf, size_t size) = 0;
    virtual size_t list_contents(const char *path, void* buf, size_t size, file_offset *offset = 0) = 0;
    virtual void close_file(file* descriptor) = 0;
    virtual bool stat(const char *path, fs_stat *out_stat) = 0;
    virtual bool truncate(file *descriptor, size_t size) = 0;
};