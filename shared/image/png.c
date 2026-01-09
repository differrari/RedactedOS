#include "png.h"
#include "image.h"
#include "syscalls/syscalls.h"
#include "std/memory_access.h"
#include "std/memory.h"
#include "math/math.h"
#include "compression/deflate.h"

typedef struct png_chunk_hdr {
    uint32_t length;
    char type[4];
    //Followed by data and 4 byte crc
} png_chunk_hdr;

typedef struct png_ihdr {
    uint32_t width;
    uint32_t height;
    uint8_t depth;// (1 byte, values 1, 2, 4, 8, or 16)
    uint8_t color_type;// (1 byte, values 0, 2, 3, 4, or 6)
    uint8_t compression;// (1 byte, value 0)
    uint8_t filter;// (1 byte, value 0)
    uint8_t interlace;// (1 byte, values 0 "no interlace" or 1 "Adam7 interlace")
} png_ihdr;

image_info png_get_info(void * file, size_t size){
    uint64_t header = *(uint64_t*)file;
    if (header != 0xA1A0A0D474E5089){
        printf("Wrong PNG header %x",header);
        return (image_info){0,0};
    }
    uintptr_t p = (uintptr_t)file + sizeof(uint64_t);
    png_chunk_hdr *hdr = (png_chunk_hdr*)p;
    if (strstart_case(hdr->type, "IHDR",true) != 4){
        printf("Couldn't find png IHDR");
        return (image_info){0,0};
    }
    p += sizeof(png_chunk_hdr);
    png_ihdr *ihdr = (png_ihdr*)p;
    //Check the crc
    return (image_info){__builtin_bswap32(ihdr->width),__builtin_bswap32(ihdr->height)};
}   

uint8_t paeth_byte(uint8_t a, uint8_t b, uint8_t c){
    int16_t p = a + b - c;
    uint16_t pa = abs(p - a);
    uint16_t pb = abs(p - b);
    uint16_t pc = abs(p - c);

    if (pa <= pb && pa <= pc)
        return a % 256;
    else if (pb <= pc)
        return b % 256;
    else
        return c % 256;
}

uint32_t paeth_predict(uint32_t a, uint32_t b, uint32_t c) {
    uint8_t alpha = paeth_byte((a >> 24) & 0xFF, (b >> 24) & 0xFF, (c >> 24) & 0xFF);
    uint8_t red = paeth_byte((a >> 16) & 0xFF, (b >> 16) & 0xFF, (c >> 16) & 0xFF);
    uint8_t green = paeth_byte((a >> 8) & 0xFF, (b >> 8) & 0xFF, (c >> 8) & 0xFF);
    uint8_t blue = paeth_byte(a & 0xFF, b & 0xFF, c & 0xFF);
    return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

uint32_t png_filter_apply(uint32_t a, uint32_t b){
    uint8_t alpha = (((a >> 24) & 0xFF) + ((b >> 24) & 0xFF)) % 256;
    uint8_t red = (((a >> 16) & 0xFF) + ((b >> 16) & 0xFF)) % 256;
    uint8_t green = (((a >> 8) & 0xFF) + ((b >> 8) & 0xFF)) % 256;
    uint8_t blue = ((a & 0xFF) + (b & 0xFF)) % 256;
    return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

uint32_t png_average_apply(uint32_t a, uint32_t b){
    uint8_t alpha = (((a >> 24) & 0xFF) + ((b >> 24) & 0xFF))/2;
    uint8_t red = (((a >> 16) & 0xFF) + ((b >> 16) & 0xFF))/2;
    uint8_t green = (((a >> 8) & 0xFF) + ((b >> 8) & 0xFF))/2;
    uint8_t blue = ((a & 0xFF) + (b & 0xFF))/2;
    return ((alpha % 256) << 24) | ((red % 256) << 16) | ((green % 256) << 8) | (blue % 256);
}

void png_process_raw(uintptr_t raw_img, uint32_t w, uint32_t h, uint16_t bpp, uint32_t *buf){
    const uint8_t bytes = bpp/8;
    for (uint32_t y = 0; y < h; y++){
        uint8_t filter_type = *(uint8_t*)(raw_img + (((w * bytes) + 1) * y));
        for (uint32_t x = 0; x < w; x++){
            uint32_t current = get_color_bpp(bpp, (raw_img + (((w * bytes) + 1) * y) + 1 + (x*bytes)));
            // if (y == 0) printf("%.8x",current);
            switch (filter_type) {
                case 0: 
                break;
                case 1:
                    if (x > 0) current = png_filter_apply(current,buf[(y * w) + x - 1]);
                    break;
                case 2:
                    if (y > 0) current = png_filter_apply(current,buf[((y-1) * w) + x]);
                    break;
                case 3: {
                    uint32_t raw = x == 0 ? 0 : buf[(y * w) + x - 1];
                    uint32_t prior = y == 0 ? 0 : buf[((y-1) * w) + x];
                    current = png_filter_apply(current, png_average_apply(raw,prior));
                    break;
                }
                case 4: {
                    uint32_t prev = x == 0 ? 0 : buf[(y * w) + x - 1];
                    uint32_t top = y == 0 ? 0 : buf[((y - 1) * w) + x];
                    uint32_t diag = y == 0 || x == 0 ? 0 : buf[((y - 1) * w) + x - 1];
                    current = png_filter_apply(current, paeth_predict(prev, top, diag));
                    break;
                }
            }
            buf[(y * w) + x] = current; 
        }
    }
    for (uint32_t i = 0; i < h*w; i++) buf[i] = convert_bpp_color(bpp, buf[i]);
}

uint16_t png_decode_bpp(png_ihdr *ihdr){
    switch (ihdr->color_type) {
        case 0: return ihdr->depth;//Greyscale
        case 2: return 3 * ihdr->depth;//TrueColor
        case 3: return ihdr->depth;//IndexedColor
        case 4: return 2 * ihdr->depth;//GrayScale with Alpha
        case 6: return 4 * ihdr->depth;//Truecolor with alpha
    }
    return 0;
}

void png_read_image(void *file, size_t size, uint32_t *buf){
    uint64_t header = *(uint64_t*)file;
    if (header != 0xA1A0A0D474E5089){
        printf("Wrong PNG header %x",header);
        return;
    }
    printf("File size %x",size);
    uintptr_t p = (uintptr_t)file + sizeof(uint64_t);
    png_chunk_hdr *hdr;
    image_info info = {};
    uintptr_t out_buf = 0;
    void *data_buf = 0;
    uintptr_t data_cursor = 0;
    uint16_t bpp = 0;
    deflate_read_ctx ctx = {};
    do {
        hdr = (png_chunk_hdr*)p;
        uint32_t length = __builtin_bswap32(hdr->length);
        if (strstart_case(hdr->type, "IHDR",true) == 4){
            png_ihdr *ihdr = (png_ihdr*)(p + sizeof(png_chunk_hdr));
            bpp = png_decode_bpp(ihdr);
            info = (image_info){__builtin_bswap32(ihdr->width),__builtin_bswap32(ihdr->height)};
        }
        if (strstart_case(hdr->type, "IDAT",true) == 4){
            if (info.width == 0 || info.height == 0){
                printf("Wrong image size");
                return;
            }
            if (!out_buf){ 
                out_buf = (uintptr_t)malloc((info.width * info.height * system_bpp) + info.height);//TODO: bpp might be too big, read image format
                ctx.output_buf = (uint8_t*)out_buf;
            }
            if (!data_buf){
                data_buf = malloc(size);
            }
            memcpy((void*)((uintptr_t)data_buf + data_cursor), (void*)(p + sizeof(png_chunk_hdr)), length);
            data_cursor += length;
            // printf("Found some idat %x - %x",p + sizeof(png_chunk_hdr) - (uintptr_t)file, length);
        }
        p += sizeof(png_chunk_hdr) + __builtin_bswap32(hdr->length) + sizeof(uint32_t);
    } while(strstart_case(hdr->type, "IEND",true) != 4);
    deflate_decode(data_buf, data_cursor, &ctx);
    png_process_raw(out_buf, info.width, info.height, bpp, buf);
}

void* load_png(char *path, image_info *info){
    file descriptor = {};
    FS_RESULT res = openf(path, &descriptor);
    void *img = 0;
    void* file_img = malloc(descriptor.size);
    readf(&descriptor, file_img, descriptor.size);
    if (res != FS_RESULT_SUCCESS){ 
        closef(&descriptor);
        printf("Couldn't open image");
        return 0;
    }
    closef(&descriptor);
    *info = png_get_info(file_img, descriptor.size);
    printf("info %ix%i",info->width,info->height);
    img = malloc(info->width*info->height*system_bpp);
    png_read_image(file_img, descriptor.size, img);
    return img;
}
