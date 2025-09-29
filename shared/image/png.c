#include "png.h"
#include "syscalls/syscalls.h"
#include "std/memory_access.h"
#include "compression/huffman.h"

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

typedef union zlib_hdr {
    struct {
        uint16_t cm: 4;// 8 = DEFLATE
        uint16_t cinf : 4;

        uint16_t fcheck: 1;//checksum
        uint16_t dict: 1;
        uint16_t flevel : 2;
        
        uint16_t rsvd: 4;
    };
    uint16_t hdr;
}__attribute__((packed)) zlib_hdr;

typedef union deflate_dynamic_hdr {
    struct {
        uint32_t bfinal: 1;
        uint32_t btype: 2;
        uint32_t hlit: 5;
        
        uint32_t hdist: 5;
        uint32_t hclen1: 3;

        uint32_t hclen2: 1;
        uint32_t rsvd: 7;

        uint32_t rsvd2: 8;
    };
    uint32_t value;
} deflate_dynamic_hdr;

void png_load_idat(void* ptr, size_t size){
    zlib_hdr hdr = *(zlib_hdr*)ptr;
    if (hdr.cm != 8){
        printf("Only DEFLATE is supported");
    }
    uintptr_t p = (uintptr_t)ptr + sizeof(zlib_hdr);
    //TODO: move to deflate header
    deflate_dynamic_hdr dyn_hdr = (deflate_dynamic_hdr)read_unaligned32((uint32_t*)p);
    uint8_t hclen = (dyn_hdr.hclen1) | (dyn_hdr.hclen2 << 3);
    printf("HCLEN %b %b",dyn_hdr.hclen1,dyn_hdr.hclen2);
    printf("DEFLATE DYNAMIC HEADER. LAST? %i. Type %i. HLIT %i, HDIST %i, HCLEN2 %i", dyn_hdr.bfinal, dyn_hdr.btype, dyn_hdr.hlit, dyn_hdr.hdist, hclen);

    p += 2;
    uint8_t bs = 1;
    uint8_t *bytes = (uint8_t*)p;
    int c = 0;
    uint8_t code_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t permuted[19];
    for (uint8_t i = 0; i < hclen+4; i++){
        if (bs > 5)
            permuted[code_order[i]] = (bytes[c+1] << (8-bs) & 0b111) | ((bytes[c] >> bs) & 0b111);
        else 
            permuted[code_order[i]] = (bytes[c] >> bs) & 0b111;
        printf("[%i] = %i",code_order[i],permuted[code_order[i]]);
        bs += 3;
        if (bs > 7){
            bs %= 8;
            c++;
        }
    }
    //Generalize this part to construct a huffman tree from a symbol:code_length kvp
    uint8_t bl_count[8];
    for (int i = 0; i < 8; i++){
        for (int j = 0; j < 19; j++){
            if (permuted[j] == i) bl_count[i]++;
        }
        printf("%i appears %i times",i,bl_count[i]);
    }
    int next_code[9];
    int code = 0;
    bl_count[0] = 0;
    for (int bits = 1; bits <= 8; bits++) {
        printf("Bit %i: %i (i-1) appeared %i times",bits, bits-1, bl_count[bits-1]);
        code = (code + bl_count[bits-1]) << 1;
        next_code[bits] = code;
        printf("Next code [%i] = %i",bits,next_code[bits]);
    }
    huff_tree_node *root = malloc(sizeof(huff_tree_node));
    for (int i = 0; i < 19; i++){
        if (permuted[i])
            huffman_populate(root, next_code[permuted[i]]++, permuted[i], i);
    }
    huffman_viz(root, 0, 0);
}

image_info png_get_info(void * file, size_t size){
    uint64_t header = *(uint64_t*)file;
    if (header != 0xA1A0A0D474E5089){
        printf("Wrong PNG header %x",header);
        return (image_info){0,0};
    }
    uintptr_t p = (uintptr_t)file + sizeof(uint64_t);
    png_chunk_hdr *hdr = (png_chunk_hdr*)p;
    if (strstart(hdr->type, "IHDR", true) != 4){
        printf("Couldn't find png IHDR");
        return (image_info){0,0};
    }
    p += sizeof(png_chunk_hdr);
    png_ihdr *ihdr = (png_ihdr*)p;
    //Check the crc
    return (image_info){__builtin_bswap32(ihdr->width),__builtin_bswap32(ihdr->height)};
}

void png_read_image(void *file, size_t size, uint32_t *buf){
uint64_t header = *(uint64_t*)file;
    if (header != 0xA1A0A0D474E5089){
        printf("Wrong PNG header %x",header);
        return;
    }
    uintptr_t p = (uintptr_t)file + sizeof(uint64_t);
    png_chunk_hdr *hdr;
    do {
        hdr = (png_chunk_hdr*)p;
        uint32_t length = __builtin_bswap32(hdr->length);
        if (strstart(hdr->type, "IDAT", true) == 4){
            printf("Found some idat %x",p + sizeof(png_chunk_hdr) - (uintptr_t)file);
            png_load_idat((void*)(p + sizeof(png_chunk_hdr)), length);
            return;
        }
        p += sizeof(png_chunk_hdr) + __builtin_bswap32(hdr->length) + sizeof(uint32_t);
    } while(strcmp(hdr->type, "IEND", true));
}