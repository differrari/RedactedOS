#pragma once

#include "types.h"
#include "files/fs.h"

#define VERSION_NUM(major,minor,patch,build) (uint64_t)((((uint64_t)major) << 48) | (((uint64_t)minor) << 32) | (((uint64_t)patch) << 16) | ((uint64_t)build))

#ifdef __cplusplus
extern "C" {
#endif
uint64_t reserve_fd_id();
uint64_t reserve_fd_gid(const char *path);
#ifdef __cplusplus
}
#endif

typedef struct system_module {
    const char* name;
    const char* mount;
    uint64_t version;

    bool (*init)();
    bool (*fini)();

    FS_RESULT (*open)(const char*, file*);
    size_t (*read)(file*, char*, size_t, file_offset);
    size_t (*write)(file*, const char *, size_t, file_offset);
    void (*close)(file *descriptor);

    size_t (*sread)(const char*, void*, size_t);
    size_t (*swrite)(const char*, const void*, size_t);

    size_t (*readdir)(const char*, void*, size_t, file_offset*);

} system_module;

typedef struct module_file {
    uint64_t fid;
    size_t file_size;
    uintptr_t buffer;
    bool ignore_cursor;
    bool read_only;
    uint64_t references;
} module_file;
//TODO: for IPC create a dedicated loading function that attaches a module to a process so it can be cleaned up