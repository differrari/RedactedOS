#pragma once

#define FD_INVALID 0
#define FD_IN 1
#define FD_OUT 2

typedef struct file {
    uint64_t id;
    size_t size;
    uint64_t cursor;
} file;

typedef uint64_t file_offset;

typedef enum SEEK_TYPE {
    SEEK_ABSOLUTE,
    SEEK_RELATIVE
} SEEK_TYPE;

typedef enum FS_RESULT {
    FS_RESULT_DRIVER_ERROR,
    FS_RESULT_SUCCESS,
    FS_RESULT_NOTFOUND,
} FS_RESULT;