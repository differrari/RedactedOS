#pragma once

#include "data/struct/hashmap.h"
#include "files/system_module.h"
#include "fs_isolation.h"
#include "files/dir_list.h"

bool load_module_to(module_root* modules, system_module *module);
bool unload_module_from(module_root* modules, system_module *module);
system_module* get_module_from(module_root* modules, const char **full_path);
size_t list_root_from(module_root* modules, fs_dir_list_helper *helper, uint64_t *offset);
bool root_stat(const char *path, fs_stat *out_stat);

extern system_module root_module; 