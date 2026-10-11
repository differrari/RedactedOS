#pragma once

#include "files/system_module.h"

void register_procfs(u16 procid);

typedef struct {
    buffer output;
    buffer state;
    buffer info;
    buffer id;
    buffer input;
} procfs_files;

u64 resolve_reserved_fd(u64 fd);

extern system_module procfs_mod;