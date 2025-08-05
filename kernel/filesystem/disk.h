#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

bool init_disk_device();
void disk_verbose();

void disk_write(const void *buffer, uint32_t sector, uint32_t count);
void disk_read(void *buffer, uint32_t sector, uint32_t count);

#ifdef __cplusplus
}
#endif