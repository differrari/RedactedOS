#include "bmp.h"
#include "math/math.h"
#include "syscalls/syscalls.h"

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

image_info bmp_get_info(void * file, size_t size){
    if (size < sizeof(bmp_header)) return (image_info){0,0};
    bmp_header *header = (bmp_header*)file;
    return (image_info){
        .width = header->width,
        .height = abs(header->height)
    };
}

void bmp_read_image(void *file, size_t size, uint32_t *buf){
    if (size < sizeof(bmp_header)){ 
        printf("Wrong file size");
        return;
    }
    bmp_header *header = (bmp_header*)file;
    if (size < header->data_offset + header->img_size || size < header->file_size){ 
        printf("Wrong file size");
        return;
    }
    uintptr_t color_data = (uintptr_t)file + header->data_offset;
    uint16_t increment = header->bpp/8;
    uint32_t height = abs(header->height);
    uint32_t width = (uint32_t)header->width;
    bool flipped = header->height > 0;
    uint32_t padding = 4 - (((header->bpp/8) * width) % 4);
    if (padding == 4) padding = 0;
    uint32_t padded = ((header->bpp/8) * width) + padding;
    for (uint32_t y = 0; y < height; y++){
        for (uint32_t x = 0; x < (uint32_t)header->width; x++)
            buf[(y * header->width) + x] = get_bpp_converted_color(header->bpp, color_data + ((flipped ? height - y - 1 : y) * padded) + x * increment);   
    }
}

void* load_bmp(char *path, image_info *info){
    file descriptor = {};
    FS_RESULT res = openf(path, &descriptor);
    void *img = 0;
    image_info img_info;
    if (res == FS_RESULT_SUCCESS){
        void *img_file = (void*)malloc(descriptor.size);
        readf(&descriptor, img_file, descriptor.size);
        img_info = bmp_get_info(img_file, descriptor.size);
        closef(&descriptor);
        if (img_info.width > 0 && img_info.height > 0){
            size_t image_size = img_info.width * img_info.height * system_bpp;
            img = (void*)malloc(image_size);
            bmp_read_image(img_file, descriptor.size, img);
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
