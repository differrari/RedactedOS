#include "bmp.h"
#include "std/memory_access.h"
#include "math/math.h"

typedef struct bmp_header {
    char signature[2];
    uint32_t file_size;
    uint32_t rsvd;
    uint32_t data_offset;

    //DIB - BITMAPINFOHEADER
    uint32_t dib_size;
    int32_t width;
    int32_t height;
    uint16_t planes;//Must be 1
    uint16_t bpp;
    uint32_t compression;//Table
    uint32_t img_size;
    
    int32_t horizontal_ppm;
    int32_t vertical_ppm;
    
    uint32_t num_colors;//0 is 2^n
    uint32_t important_colors;//0 is all, ignored
}__attribute__((packed)) bmp_header;

#include "syscalls/syscalls.h"

image_info bmp_get_info(void * file, size_t size){
    bmp_header *header = (bmp_header*)file;
    printf("%x. Width %x",header, &header->width);
    return (image_info){
        .width = header->width,
        .height = header->height
    };
}

#define ARGB(a,r,g,b) ((a & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF)

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

void bmp_read_image(void *file, size_t size, uint32_t *buf){
    bmp_header *header = (bmp_header*)file;
    uintptr_t color_data = (uintptr_t)file + header->data_offset;
    uint16_t increment = header->bpp/8;
    uint32_t height = abs(header->height);
    uint32_t width = (uint32_t)header->width;
    bool flipped = header->height > 0;
    uint32_t padding = ((header->bpp * width) % 32)/8;
    for (uint32_t y = 0; y < height; y++){
        for (uint32_t x = 0; x < (uint32_t)header->width; x++)
            buf[(y * header->width) + x] = convert_color_bpp(header->bpp, color_data + (((flipped ? height - y - 1 : y) * (header->width + padding)) + x) * increment);   
    }
}
