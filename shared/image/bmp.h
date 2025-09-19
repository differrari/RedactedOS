#pragma once

#include "types.h"
#include "image.h"

#define system_bpp 32

image_info bmp_get_info(void * file, size_t size);
void bmp_read_image(void *file, size_t size, uint32_t *buf);

void* load_bmp(char *path, image_info *info);