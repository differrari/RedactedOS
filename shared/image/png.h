#pragma once

#include "types.h"
#include "image.h"

image_info png_get_info(void * file, size_t size);
void png_read_image(void *file, size_t size, uint32_t *buf);

void* load_png(char *path, image_info *info);