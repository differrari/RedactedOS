#include "png.h"
#include "syscalls/syscalls.h"
#include "std/memory_access.h"
#include "compression/huffman.h"
#include "std/memory.h"
#include "math/math.h"

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

huff_tree_node* deflate_decode_codes(uint8_t max_code_length, uint16_t alphabet_length, uint16_t lengths[]){
    uint16_t bl_count[max_code_length] = {};
    for (int i = 0; i < max_code_length; i++){
        for (int j = 0; j < alphabet_length; j++){
            if (lengths[j] == i){
                bl_count[i]++;
            } 
        }
        // printf("%i appears %i times",i,bl_count[i]);
    }
    uint16_t next_code[max_code_length+1] = {}; 
    uint16_t code = 0;
    bl_count[0] = 0;
    for (int bits = 1; bits <= max_code_length; bits++) {
        // printf("Bit %i: %i (i-1) appeared %i times",bits, bits-1, bl_count[bits-1]);
        code = (code + bl_count[bits-1]) << 1;
        next_code[bits] = code;
        // printf("Next code [%i] = %i",bits,next_code[bits]);
    }
    huff_tree_node *root = malloc(sizeof(huff_tree_node));
    for (int i = 0; i < alphabet_length; i++){
        if (lengths[i]){
            huffman_populate(root, next_code[lengths[i]]++, lengths[i], i);
        }
    }
    return root;
}

#define READ_BITS(from, to, amount, bs, c) do { \
    if (bs + amount > 16){ \
        to = (((uint16_t)from[c+2] << (16-bs)) & ((1 << amount) - 1)) |  \
             (((uint16_t)from[c+1] << (8-bs))  & ((1 << amount) - 1)) |  \
             (((uint16_t)from[c]   >> bs)      & ((1 << amount) - 1) ); \
    } else if (bs+amount > 8){ \
        to = (((uint16_t)from[c+1] << (8-bs))  & ((1 << amount) - 1)) |  \
             (((uint16_t)from[c]   >> bs)      & ((1 << amount) - 1) ); \
    } else { \
        to = (((uint16_t)from[c]   >> bs)      & ((1 << amount) - 1) ); \
    } \
    bs += amount;\
    if (bs > 7){\
        c += (bs/8);\
        bs %= 8;\
    } \
} while (0)

uint32_t base_lengths[] = {
    3, 4, 5, 6, 7, 8, 9, 10, //257 - 264
    11, 13, 15, 17,          //265 - 268
    19, 23, 27, 31,          //269 - 273 
    35, 43, 51, 59,          //274 - 276
    67, 83, 99, 115,         //278 - 280
    131, 163, 195, 227,      //281 - 284
    258                      //285
};

uint32_t dist_bases[] = {
    /*0*/ 1, 2, 3, 4,    //0-3
    /*1*/ 5, 7,          //4-5
    /*2*/ 9, 13,         //6-7
    /*3*/ 17, 25,        //8-9
    /*4*/ 33, 49,        //10-11
    /*5*/ 65, 97,        //12-13
    /*6*/ 129, 193,      //14-15
    /*7*/ 257, 385,      //16-17
    /*8*/ 513, 769,      //18-19
    /*9*/ 1025, 1537,    //20-21
    /*10*/ 2049, 3073,   //22-23
    /*11*/ 4097, 6145,   //24-25
    /*12*/ 8193, 12289,  //26-27
    /*13*/ 16385, 24577  //28-29
};

size_t deflate_decode(void* ptr, size_t size, uint8_t *output_buf){
    zlib_hdr hdr = *(zlib_hdr*)ptr;
    if (hdr.cm != 8){
        printf("Only DEFLATE is supported");
    }
    uintptr_t p = (uintptr_t)ptr + sizeof(zlib_hdr);

    uint8_t *bytes = (uint8_t*)p;
    uint8_t bs = 0;
    int c = 0;
    bool final = false;
    uintptr_t out_cursor = 0;

    while (!final){
        uint8_t hclen = 0;//4
        uint8_t hdist = 0;//5
        uint8_t hlit = 0;//5
        uint8_t btype = 0;//2
        READ_BITS(bytes, final, 1, bs, c);
        READ_BITS(bytes, btype, 2, bs, c);
        READ_BITS(bytes, hlit, 5, bs, c);
        READ_BITS(bytes, hdist, 5, bs, c);
        READ_BITS(bytes, hclen, 4, bs, c);
        printf("DEFLATE DYNAMIC HEADER. LAST? %i. Type %i. HLIT %i, HDIST %i, HCLEN %i", final, btype, hlit, hdist, hclen);
        
        uint8_t code_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        uint16_t permuted[19] = {};
        for (uint8_t i = 0; i < hclen+4; i++){
            READ_BITS(bytes, permuted[code_order[i]], 3, bs, c);
        }
        huff_tree_node *huff_decode_nodes = deflate_decode_codes(8, 19, permuted);
        
        huffman_viz(huff_decode_nodes, 0, 0);
        
        int tree_data_size = hlit + hdist + 258;

        printf("Expecting to read %i",tree_data_size);

        huff_tree_node *tree_root = huff_decode_nodes;

        uint16_t *full_huffman = malloc(sizeof(uint16_t) * tree_data_size);

        uint16_t last_code_len = 0;

        for (int i = 0; i < tree_data_size;){
            uint8_t next_bit = 0;
            READ_BITS(bytes, next_bit, 1, bs, c);
            tree_root = huffman_traverse(tree_root, next_bit);
            if (!tree_root) {
                printf("DEFLATE ERROR: no tree found");
                return 0;
            }
            if (!tree_root->left && !tree_root->right){
                uint8_t extra = 0;
                switch (tree_root->entry){
                    case 16:
                    READ_BITS(bytes, extra, 2, bs, c);
                    extra += 3;
                    for (int j = 0; j < extra; j++)
                        full_huffman[i+j] = last_code_len;
                    break;
                    case 17:
                    READ_BITS(bytes, extra, 3, bs, c);
                    extra += 3;
                    for (int j = 0; j < extra; j++)
                        full_huffman[i+j] = 0;
                    break;
                    case 18:
                    READ_BITS(bytes, extra, 7, bs, c);
                    extra += 11;
                    for (int j = 0; j < extra; j++)
                        full_huffman[i+j] = 0;
                    break;
                    default:
                    full_huffman[i] = tree_root->entry;
                    last_code_len = tree_root->entry;
                    break;
                }
                tree_root = huff_decode_nodes;
                if (extra) 
                    i+= extra;
                else i++;
            }
        }
        // huffman_free(huff_decode_nodes);

        printf("**** LITERAL/LENGTH ****");
        huff_tree_node *litlen_tree = deflate_decode_codes(15, hlit + 257, full_huffman);
        huffman_viz(litlen_tree, 0, 0);

        printf("**** DISTANCE ****");
        huff_tree_node *dist_tree = deflate_decode_codes(15, hdist + 1, full_huffman + hlit + 257);

        huffman_viz(dist_tree, 0, 0);
        // printf("**** WOO ****");

        // free(full_huffman, tree_data_size * sizeof(uint16_t));

        tree_root = litlen_tree;

        printf("Compressed data at %x",bytes + c);
        
        uint16_t val = 0;
        while (val != 0x100){
            uint8_t next_bit = 0;
            READ_BITS(bytes, next_bit, 1, bs, c);
            printf("%x",next_bit);
            tree_root = huffman_traverse(tree_root, next_bit);
            if (!tree_root) {
                printf("DEFLATE ERROR: no tree found");
                return 0;
            }
            if (!tree_root->left && !tree_root->right){
                val = tree_root->entry;
                if (val < 0x100){
                    output_buf[out_cursor] = (val & 0xFF);
                    printf("Literal %x",output_buf[out_cursor]);
                    out_cursor++;
                } else if (val == 0x100){
                    break;
                } else {
                    uint8_t extra = 0;
                    if (val > 264 && val < 285)
                        extra = (val-261)/4;
                    uint16_t extra_val = 0;
                    READ_BITS(bytes, extra_val, extra, bs, c);
                    uint16_t length = base_lengths[val - 257] + extra_val;
                    huff_tree_node *dist_node = dist_tree;
                    while (dist_node->left || dist_node->right){
                        READ_BITS(bytes, next_bit, 1, bs, c);
                        dist_node = huffman_traverse(dist_node, next_bit);
                        if (!dist_node){
                            printf("DEFLATE ERROR: no tree found");
                            return 0;
                        }
                    }
                    uint16_t dist_base = dist_node->entry;
                    uint8_t extra_dist = 0;
                    uint16_t extra_dist_val = 0;
                    if (dist_base > 3){
                        extra_dist = (dist_base-2)/2;
                        READ_BITS(bytes, extra_dist_val, extra_dist, bs, c);
                    }
                    // printf("Dista base %i + extra %i",dist_base,extra_dist);
                    uint32_t distance = dist_bases[dist_base] + extra_dist_val;
                    // printf("Last %x",output_buf[out_cursor-1]);
                    // printf("Copying %i bytes from %i bytes back. %x + %x",length,distance,output_buf, out_cursor);
                    memcpy(output_buf - distance + out_cursor, output_buf + out_cursor, length);
                    printf("Sanity check 1 %x",output_buf[distance + out_cursor]);
                    out_cursor += length;
                    // printf("Sanity check 2 %x",output_buf[out_cursor-1]);
                }
                tree_root = litlen_tree;
            }
        }
    }

    printf("Wrote a total of %i bytes",out_cursor);

    // huffman_free(litlen_tree);
    // huffman_free(dist_tree);

    return out_cursor;
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
    printf("File size %x",size);
    uintptr_t p = (uintptr_t)file + sizeof(uint64_t);
    png_chunk_hdr *hdr;
    image_info info = {};
    uintptr_t out_buf = 0;
    uintptr_t out_off = 0;
    do {
        hdr = (png_chunk_hdr*)p;
        uint32_t length = __builtin_bswap32(hdr->length);
        if (strstart(hdr->type, "IHDR", true) == 4){
            png_ihdr *ihdr = (png_ihdr*)(p + sizeof(png_chunk_hdr));
            info = (image_info){__builtin_bswap32(ihdr->width),__builtin_bswap32(ihdr->height)};
        }
        if (strstart(hdr->type, "IDAT", true) == 4){
            if (info.width == 0 || info.height == 0){
                printf("Wrong image size");
                return;
            }
            if (!out_buf) out_buf = (uintptr_t)malloc(info.width * info.height * system_bpp);//TODO: bpp might be too big, read image format
            printf("Found some idat %x",p + sizeof(png_chunk_hdr) - (uintptr_t)file);
            uintptr_t prev_off = out_off;
            out_off += deflate_decode((void*)(p + sizeof(png_chunk_hdr)), length, (uint8_t*)(out_buf + out_off));
            memcpy(buf + prev_off, (void*)out_buf, min(out_off, size-prev_off));
            // for (uint32_t i = out_off-1; i < out_off ; i--){
            printf("%x", buf[prev_off]);
            printf("%x", buf[prev_off+1]);
            // }
            return;
        }
        p += sizeof(png_chunk_hdr) + __builtin_bswap32(hdr->length) + sizeof(uint32_t);
    } while(strstart(hdr->type, "IEND", true) != 4);
}