#pragma once

#include "types.h"
#include "files/fs.h"
#include "files/buffer.h"
#include "files/system_module.h"

#ifdef __cplusplus
extern "C" {
#endif
uint64_t reserve_fd_id();
uint64_t reserve_fd_gid(const char *path);
#ifdef __cplusplus
}
#endif

typedef struct module_file {
    const char* name;
    fs_backing_type backing_type;
    fs_entry_type entry_type;
    uint64_t fid;
    
    uint64_t serial;
    
    uptr buf;
    bool ignore_cursor;
    bool read_only;
    size_t file_size;
    
    buffer file_buffer;
    uint64_t references;
} module_file;