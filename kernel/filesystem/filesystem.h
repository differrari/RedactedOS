#pragma once

#include "types.h"
#include "std/string.h"
#include "dev/driver_base.h"

#ifdef __cplusplus
extern "C" {
#endif

FS_RESULT open_file(const char* path, file* descriptor);
size_t read_file(file *descriptor, char* buf, size_t size);
size_t write_file(file *descriptor, const char* buf, size_t size);
void close_file(file *descriptor);
sizedptr list_directory_contents(const char *path);
bool init_filesystem();

#ifdef __cplusplus
}
#endif