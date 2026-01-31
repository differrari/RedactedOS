#pragma once

#include "types.h"
#include "files/fs.h"
#include "files/buffer.h"
#include "files/system_module.h"

#define VERSION_NUM(major,minor,patch,build) (uint64_t)((((uint64_t)major) << 48) | (((uint64_t)minor) << 32) | (((uint64_t)patch) << 16) | ((uint64_t)build))

#ifdef __cplusplus
extern "C" {
#endif
uint64_t reserve_fd_id();
uint64_t reserve_fd_gid(const char *path);
#ifdef __cplusplus
}
#endif

typedef struct module_file {
    uint64_t fid;
    size_t file_size;
    uintptr_t buf;//TODO: remove this (and other options)
    bool ignore_cursor;
    bool read_only;
    buffer file_buffer;
    uint64_t references;
} module_file;
//TODO: for IPC create a dedicated loading function that attaches a module to a process so it can be cleaned up