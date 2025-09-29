#pragma once

#include "types.h"

typedef enum IMAGE_FORMATS {
    BMP,
    PNG
} IMAGE_FORMATS;

#define ARGB(a,r,g,b) ((a & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF)
#define system_bpp 32

typedef struct image_info {
    uint32_t width, height;
} image_info;

uint32_t convert_color_bpp(uint16_t bpp, uintptr_t value_ptr);
void* load_image(char *path, image_info *info, IMAGE_FORMATS format);