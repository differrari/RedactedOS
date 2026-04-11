#pragma once

#include "driver_base.h"
#include "data/struct/linked_list.h"

#ifdef __cplusplus 
extern "C" {
#endif

bool load_module(system_module *module);
bool unload_module(system_module *module);
system_module* get_module(const char **full_path);
size_t list_root(void* buf, size_t size, uint64_t *offset);

extern linked_list_t* modules;

#ifdef __cplusplus 
}
#endif