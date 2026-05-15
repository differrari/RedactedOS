#pragma once

#include "data/struct/hashmap.h"
#include "files/system_module.h"
#include "fs_isolation.h"

bool load_module_to(module_root* modules, system_module *module);
bool unload_module_from(module_root* modules, system_module *module);
system_module* get_module_from(module_root* modules, const char **full_path);
size_t list_root_from(module_root* modules, void* buf, size_t size, uint64_t *offset);