#pragma once

#include "types.h"

#define VIRTIO_BLK_ID 0x1001

#ifdef __cplusplus
extern "C" {
#endif
//TODO port to c++
bool vblk_find_disk();
void vblk_disk_verbose();
void vblk_write(const void *buffer, uint32_t sector, uint32_t count);
void vblk_read(void *buffer, uint32_t sector, uint32_t count);
#ifdef __cplusplus
}
#endif