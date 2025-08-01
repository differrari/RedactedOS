#pragma once

#include "driver_base.h"
#include "data_struct/linked_list.h"

#ifdef __cplusplus 
extern "C" {
#endif

bool load_module(driver_module module);
bool unload_module(driver_module module);
driver_module* get_module(const char **full_path);

extern clinkedlist_t* modules;

#ifdef __cplusplus 
}
#endif