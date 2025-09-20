#pragma once

#include "types.h"

#define ARGB(a,r,g,b) ((a & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF)

typedef struct image_info {
    uint32_t width, height;
} image_info;

uint32_t convert_color_bpp(uint16_t bpp, uintptr_t value_ptr);