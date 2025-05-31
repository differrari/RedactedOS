#pragma once

#include "types.h"
#include "dtb.h"

bool vblk_find_disk();
void vblk_disk_verbose();
void vblk_write(const void *buffer, uint32_t sector, uint32_t count);
void vblk_read(void *buffer, uint32_t sector, uint32_t count);