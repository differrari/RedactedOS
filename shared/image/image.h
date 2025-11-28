#pragma once

#include "types.h"

typedef enum IMAGE_FORMATS {
    BMP,
    PNG
} IMAGE_FORMATS;

#define ARGB(a,r,g,b) ((a & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF)
#define system_bpp 4

typedef struct image_info {
    uint32_t width, height;
} image_info;

//TODO: expand image conversion to handle all cases (palette, grayscale, gs-alpha...)
uint32_t get_bpp_converted_color(uint16_t bpp, uintptr_t value_ptr);
uint32_t get_color_bpp(uint16_t bpp, uintptr_t value_ptr);
uint32_t convert_bpp_color(uint16_t bpp, uint32_t color);
void* load_image(char *path, image_info *info, IMAGE_FORMATS format);

void* load_image_resized(char *path, image_info *info, IMAGE_FORMATS format, uint32_t new_width, uint32_t new_height);

void rescale_image(uint32_t old_width, uint32_t old_height, uint32_t new_width, uint32_t new_height, uint32_t *old_img, uint32_t* new_img);