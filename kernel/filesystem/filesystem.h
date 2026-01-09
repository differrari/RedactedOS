#pragma once

#include "types.h"
#include "std/string.h"
#include "dev/driver_base.h"

#ifdef __cplusplus
extern "C" {
#endif

FS_RESULT open_file_global(const char* path, file* descriptor, system_module **mod);
FS_RESULT open_file(const char* path, file* descriptor);
size_t read_file(file *descriptor, char* buf, size_t size);
size_t write_file(file *descriptor, const char* buf, size_t size);
void close_file_global(file *descriptor, system_module *mod);
void close_file(file *descriptor);
size_t list_directory_contents(const char *path, void* buf, size_t size, uint64_t *offset);
bool init_filesystem();

size_t simple_read(const char *path, void *buf, size_t size);
size_t simple_write(const char *path, const void *buf, size_t size);

void close_files_for_process(uint16_t pid);

#ifdef __cplusplus
}
#endif