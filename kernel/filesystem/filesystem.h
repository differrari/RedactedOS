#pragma once

#include "types.h"
#include "std/string.h"
#include "files/system_module.h"
#include "files/module_loader.h"
#include "access/resources.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    u64 mfile_id;
    u64 file_id;
    size_t file_size;
    u16 pid;
    system_module* mod;
    resource resource_id;
    u64 last_sync; 
} open_file_descriptors;

FS_RESULT open_file_global(module_root *root, const char* path, file* descriptor, system_module **mod);
FS_RESULT open_file(module_root *root, const char* path, file* descriptor);
FS_RESULT instance_local_fd(system_module *mod, file *descriptor);
size_t read_file(file *descriptor, char* buf, size_t size);
size_t transform_file(module_root *root, const char* path, void* buf, size_t size);
size_t write_file(file *descriptor, const char* buf, size_t size);
void close_file_global(file *descriptor, system_module *mod);
void close_file(file *descriptor);
size_t list_directory_contents(module_root *root, const char *path, void* buf, size_t size, uint64_t *offset);
bool init_filesystem();

void sync_file(file *descriptor, bool replace);

bool get_stat(module_root *root, const char *path, fs_stat *out_stat);
bool truncate(file *descriptor, size_t size);

size_t simple_read(module_root *root, const char *path, void *buf, size_t size);
size_t simple_write(module_root *root, const char *path, const void *buf, size_t size, bool append);

void close_files_for_process(uint16_t pid);

#ifdef __cplusplus
}
#endif