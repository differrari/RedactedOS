#pragma once

#include "types.h"

typedef struct image_info {
    uint32_t width, height;
} image_info;

#define system_bpp 32

image_info bmp_get_info(void * file, size_t size);
void bmp_read_image(void *file, size_t size, uint32_t *buf);