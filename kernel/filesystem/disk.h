#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "files/system_module.h"

bool init_disk_device();
void disk_verbose();

void disk_write(const void *buffer, uint32_t sector, uint32_t count);
void disk_read(void *buffer, uint32_t sector, uint32_t count);

extern system_module disk_module;

#ifdef __cplusplus
}
#endif