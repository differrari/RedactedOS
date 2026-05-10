#pragma once

#include "data/struct/linked_list.h"
#include "files/system_module.h"

typedef hash_map_t module_root;

u64 register_fs_id();
hash_map_t* get_fs_for_id(u64 id);
hash_map_t* kernel_fs();

#ifdef __cplusplus 
extern "C" {
#endif

//Root/Kernel
bool load_module(system_module *module);
bool unload_module(system_module *module);
system_module* get_module(const char **full_path);
size_t list_root(void* buf, size_t size, uint64_t *offset);

//Userland
string resolve_isolated_path(const char *path, u64 id, module_root *resolved);

#ifdef __cplusplus 
}
#endif