#pragma once

#include "driver_base.h"
#include "data_struct/linked_list.h"

#ifdef __cplusplus 
extern "C" {
#endif

bool load_module(system_module *module);
bool unload_module(system_module *module);
system_module* get_module(const char **full_path);

extern clinkedlist_t* modules;

#ifdef __cplusplus 
}
#endif