#pragma once

#include "files/system_module.h"

#ifdef FEATURE_NEW_PROCFS
void register_procfs(u16 procid);
#endif

typedef struct {
    buffer output;
    buffer state;
    buffer info;
} procfs_files;


extern system_module procfs_mod;