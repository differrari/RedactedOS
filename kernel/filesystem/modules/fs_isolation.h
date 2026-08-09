#pragma once

#include "data/struct/linked_list.h"
#include "files/system_module.h"

typedef struct {
    hash_map_t *map;
    hash_map_t *reserved;
} module_root;

u64 register_fs_id();
module_root* get_fs_for_id(u64 id);
module_root* kernel_fs();

#ifdef __cplusplus 
extern "C" {
#endif

//Root/Kernel
bool load_module(system_module *module);
bool unload_module(system_module *module);
system_module* get_module(const char **full_path);
size_t list_root(void* buf, size_t size, uint64_t *offset);

//Userland
string resolve_isolated_path(const char *path, u64 id, module_root *resolved, bool allow_kfs);
void destroy_fs(u64 fsid);

#ifdef __cplusplus 
}
#endif