#include "image.h"
#include "std/memory_access.h"
#include "syscalls/syscalls.h"
#include "bmp.h"
#include "png.h"

uint32_t convert_color_bpp(uint16_t bpp, uintptr_t value_ptr){
    switch (bpp) {
        case 1: return 0;

        case 4: return 0;

        case 8: return 0;

        case 24: return ARGB(0xFF, read8(value_ptr + 2), read8(value_ptr + 1), read8(value_ptr)); 

        case 32: return value_ptr % 8 == 0 ? *(uint32_t*)value_ptr : read_unaligned32((uint32_t*)value_ptr);
    }
    return 0;
}

void* load_image(char *path, image_info *info, IMAGE_FORMATS format){
    file descriptor;
    printf(path);
    FS_RESULT res = fopen(path, &descriptor);
    void *img;
    image_info img_info;
    if (res == FS_RESULT_SUCCESS){
        void *img_file = (void*)malloc(descriptor.size);
        fread(&descriptor, img_file, descriptor.size);
        switch (format) {
            case PNG:
            img_info = png_get_info(img_file, descriptor.size);
            break;
            case BMP:
            img_info = bmp_get_info(img_file, descriptor.size);
            break;
            //Unknown can be handled by reading magic bytes
        }
        fclose(&descriptor);
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