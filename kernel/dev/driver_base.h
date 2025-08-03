#pragma once

#include "types.h"
#include "std/string.h"

typedef struct file {
    uint64_t id;
    size_t size;
} file;

typedef uint64_t file_offset;

typedef enum FS_RESULT {
    FS_RESULT_SUCCESS,
    FS_RESULT_NOTFOUND,
    FS_RESULT_DRIVER_ERROR,
} FS_RESULT;

#define VERSION_NUM(major,minor,patch,build) (uint64_t)((((uint64_t)major) << 48) | (((uint64_t)minor) << 32) | (((uint64_t)patch) << 16) | ((uint64_t)build))

#ifdef __cplusplus
extern "C" {
#endif
uint64_t reserve_fd_id();
#ifdef __cplusplus
}
#endif

typedef struct driver_module {
    const char* name;
    const char* mount;
    uint64_t version;

    bool (*init)();
    bool (*fini)();

    FS_RESULT (*open)(const char*, file*);
    size_t (*read)(file*, char*, size_t, file_offset);
    size_t (*write)(file*, const char *, size_t, file_offset);

    file_offset (*seek)(file*, file_offset);
    sizedptr (*readdir)(const char* path);

    //TODO: flush
    //TODO: poll
} driver_module;
