#include "image.h"
#include "std/memory_access.h"
#include "syscalls/syscalls.h"
#include "bmp.h"
#include "png.h"

uint32_t get_color_bpp(uint16_t bpp, uintptr_t value_ptr){
    switch (bpp) {
        case 1: return 0;

        case 4: return 0;

        case 8: return 0;

        case 24: return ARGB(0, read8(value_ptr + 0), read8(value_ptr + 1), read8(value_ptr + 2)); 

        case 32: {
            uint32_t pix = value_ptr % 8 == 0 ? *(uint32_t*)value_ptr : read_unaligned32((uint32_t*)value_ptr);
            pix = (((pix >> 24) & 0xFF) << 24) | (((pix >> 0) & 0xFF) << 16) | (((pix >> 8) & 0xFF) << 8) | (((pix >> 16) & 0xFF) << 0);
            return pix;
        }
    }
    return 0;
}

uint32_t convert_bpp_color(uint16_t bpp, uint32_t color){
    switch (bpp) {
        case 1: return 0;

        case 4: return 0;

        case 8: return 0;

        case 24: return (0xFF << 24) | color; 

        case 32: return color;
    }
    return 0;
}

uint32_t get_bpp_converted_color(uint16_t bpp, uintptr_t value_ptr){
    switch (bpp) {
        case 1: return 0;

        case 4: return 0;

        case 8: return 0;

        case 24: return ARGB(0xFF, read8(value_ptr + 2), read8(value_ptr + 1), read8(value_ptr)); 

        case 32: return value_ptr % 8 == 0 ? *(uint32_t*)value_ptr : read_unaligned32((uint32_t*)value_ptr);
    }
    return 0;
}

#ifndef CROSS

void* load_image(char *path, image_info *info, IMAGE_FORMATS format){
    file descriptor = {};
    FS_RESULT res = openf(path, &descriptor);
    void *img = 0;
    image_info img_info;
    if (res == FS_RESULT_SUCCESS){
        void *img_file = (void*)malloc(descriptor.size);
        readf(&descriptor, img_file, descriptor.size);
        switch (format) {
            case PNG:
            img_info = png_get_info(img_file, descriptor.size);
            break;
            case BMP:
            img_info = bmp_get_info(img_file, descriptor.size);
            break;
            //Unknown can be handled by reading magic bytes
        }
        closef(&descriptor);
        if (img_info.width > 0 && img_info.height > 0){
            size_t image_size = img_info.width * img_info.height * system_bpp;
            img = (void*)malloc(image_size);
            switch (format) {
                case PNG:
                png_read_image(img_file, descriptor.size, img);
                break;
                case BMP:
                bmp_read_image(img_file, descriptor.size, img);
                break;
            }
            *info = img_info;
            return img;
        } else { 
            printf("Wrong image size %i",img_info.width,img_info.height);
            *info = (image_info){0, 0};
            return 0;
        }
    } else { 
        printf("Failed to open image");
        *info = (image_info){0, 0};
        return 0;
    }
}

//TODO: downsize & other interpolations
void* load_image_resized(char *path, image_info *info, IMAGE_FORMATS format, uint32_t new_width, uint32_t new_height){
    image_info old_info = {};
    void *old_img = load_image(path, &old_info, format);
    info->width = new_width;
    info->height = new_height;
    void *new_img = malloc(new_width * new_height * sizeof(uint32_t));
    if (new_width < old_info.width || new_height < old_info.height){
        printf("[IMG warning] image downscaling is not properly implemented or tested. Use at your own risk");
    }
    rescale_image(old_info.width, old_info.height, new_width, new_height, old_img, new_img);
    free_sized(old_img, old_info.width * old_info.height * sizeof(uint32_t));
    return new_img;
}

void rescale_image(uint32_t old_width, uint32_t old_height, uint32_t new_width, uint32_t new_height, uint32_t *old_img, uint32_t* new_img){
    
    for (uint32_t y = 0; y < new_height; y++){
        uint32_t oy = y * old_height/new_height;
        uint32_t *old_row = old_img + (oy * old_width);
        uint32_t *new_row = new_img + (y * new_width);
        for (uint32_t x = 0; x < new_width; x++){
            uint32_t ox = x * old_width/new_width;
            new_row[x] = old_row[ox];
        }   
    }
}

#endif
