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

typedef struct {
    uint8_t *bytes;
    uint8_t bs;
    int c;
    uintptr_t out_cursor;
    uint8_t *output_buf;
    bool final_block;
    size_t cur_block;
} deflate_read_ctx;

size_t deflate_uncommpressed(deflate_read_ctx *ctx, size_t max_size) {
    if (ctx->bs) {
        ctx->bs = 0;
        ctx->c++;
        max_size--;
    }

    // printf("Next bytes %x %x %x %x",ctx->bytes[ctx->c],ctx->bytes[ctx->c+1],ctx->bytes[ctx->c+2],ctx->bytes[ctx->c+3]);

    uint16_t len = 0;
    uint16_t nlen = 0;
    READ_BITS(ctx->bytes, len, 16, ctx->bs, ctx->c);
    READ_BITS(ctx->bytes, nlen, 16, ctx->bs, ctx->c);

    // printf("Actual read values %x %x",len, nlen);

    if (len != (uint16_t)(~nlen)) {
        printf("Wrong checksum %.16b %.16b %.16b\n", len, nlen, (uint16_t)(~nlen));
        return 0;
    }

    ctx->cur_block = len;
    printf("Found new block with %x len", len);

    max_size -= 4;

    len = min(len,max_size);

    // printf("Reading %#x bytes from uncompressed deflate block",len);

    memcpy(ctx->output_buf + ctx->out_cursor, &ctx->bytes[ctx->c], len);
    ctx->c += len;
    ctx->out_cursor += len;

    return len;
}

bool deflate_block(huff_tree_node *litlen_tree, huff_tree_node *dist_tree, deflate_read_ctx *ctx){
    huff_tree_node *tree_root = litlen_tree;
    uint16_t val = 0;
    while (val != 0x100){
        uint8_t next_bit = 0;
        READ_BITS(ctx->bytes, next_bit, 1, ctx->bs, ctx->c);
        // printf("%x",next_bit);
        tree_root = huffman_traverse(tree_root, next_bit);
        if (!tree_root) {
            printf("DEFLATE ERROR: no tree found");
            return false;
        }
        if (!tree_root->left && !tree_root->right){
            val = tree_root->entry;
            if (val < 0x100){
                ctx->output_buf[ctx->out_cursor] = (val & 0xFF);
                ctx->out_cursor++;
            } else if (val == 0x100){
                break;
            } else {
                uint8_t extra = 0;
                if (val > 264 && val < 285)
                    extra = (val-261)/4;
                uint16_t extra_val = 0;
                READ_BITS(ctx->bytes, extra_val, extra, ctx->bs, ctx->c);
                uint16_t length = base_lengths[val - 257] + extra_val;
                huff_tree_node *dist_node = dist_tree;
                while (dist_node->left || dist_node->right){
                    READ_BITS(ctx->bytes, next_bit, 1, ctx->bs, ctx->c);
                    dist_node = huffman_traverse(dist_node, next_bit);
                    if (!dist_node){
                        printf("DEFLATE ERROR: no tree found");
                        return false;
                    }
                }
                uint16_t dist_base = dist_node->entry;
                uint8_t extra_dist = 0;
                uint16_t extra_dist_val = 0;
                if (dist_base > 3){
                    extra_dist = (dist_base-2)/2;
                    READ_BITS(ctx->bytes, extra_dist_val, extra_dist, ctx->bs, ctx->c);
                }
                uint32_t distance = dist_bases[dist_base] + extra_dist_val;
                memcpy(ctx->output_buf + ctx->out_cursor, ctx->output_buf - distance + ctx->out_cursor, length);
                ctx->out_cursor += length;
            }
            tree_root = litlen_tree;
        }
    }
    return true;
}

size_t deflate_decode(void* ptr, size_t size, deflate_read_ctx *ctx){
    if (ctx->cur_block > 0){//TODO: Can a compressed deflate block cross boundaries? 
        // printf("Continuation of previous block %i %x",size, size);
        size_t amount = min(size,ctx->cur_block);
        memcpy(ctx->output_buf + ctx->out_cursor, ptr, amount);
        ctx->out_cursor += amount;
        ctx->cur_block -= amount;
        printf("Reduced current block by %x to %x",amount, ctx->cur_block);
        if (amount >= size || ctx->final_block)
            return size;
        ptr += amount;
        size -= amount;
    } else {
        zlib_hdr hdr = *(zlib_hdr*)ptr;
        if (hdr.cm != 8){
            printf("Error. Non-DEFLATE block");
            return 0;
        }
        ptr += sizeof(zlib_hdr);
        size -= sizeof(zlib_hdr);
    }
    uintptr_t p = (uintptr_t)ptr;

    bool final = false;
    
    ctx->bytes = (uint8_t*)p;
    ctx->c = 0;
    ctx->bs = 0;

    while (!final){
        uint8_t hclen = 0;//4
        uint8_t hdist = 0;//5
        uint8_t hlit = 0;//5
        uint8_t btype = 0;//2
        READ_BITS(ctx->bytes, final, 1, ctx->bs, ctx->c);
        READ_BITS(ctx->bytes, btype, 2, ctx->bs, ctx->c);

        ctx->final_block = final;

        if (btype == 0b00){
            size_t read_size = deflate_uncommpressed(ctx, size);
            // printf("Uncompressed block read %x out of a max total %x",read_size, max_size);
            if (!read_size){
                return 0;
            }
            ctx->cur_block -= read_size;
            printf("Got new block and read %x with %x remaining",read_size, ctx->cur_block);
            if (final || ctx->cur_block) return ctx->out_cursor;
            continue;
        }

        READ_BITS(ctx->bytes, hlit, 5, ctx->bs, ctx->c);
        READ_BITS(ctx->bytes, hdist, 5, ctx->bs, ctx->c);
        READ_BITS(ctx->bytes, hclen, 4, ctx->bs, ctx->c);

        if (btype != 0b10){
            printf("Only non-dynamic compression not supported right now %i",btype);
            return 0;
        }
        // printf("DEFLATE DYNAMIC HEADER. LAST? %i. Type %i. HLIT %i, HDIST %i, HCLEN %i", final, btype, hlit, hdist, hclen);
        
        uint8_t code_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        uint16_t permuted[19] = {};
        for (uint8_t i = 0; i < hclen+4; i++){
            READ_BITS(ctx->bytes, permuted[code_order[i]], 3, ctx->bs, ctx->c);
        }
        huff_tree_node *huff_decode_nodes = deflate_decode_codes(8, 19, permuted);
        
        // huffman_viz(huff_decode_nodes, 0, 0);
        
        int tree_data_size = hlit + hdist + 258;

        // printf("Expecting to read %i",tree_data_size);

        huff_tree_node *tree_root = huff_decode_nodes;

        uint16_t *full_huffman = malloc(sizeof(uint16_t) * tree_data_size);

        uint16_t last_code_len = 0;

        for (int i = 0; i < tree_data_size;){
            uint8_t next_bit = 0;
            READ_BITS(ctx->bytes, next_bit, 1, ctx->bs, ctx->c);
            tree_root = huffman_traverse(tree_root, next_bit);
            if (!tree_root) {
                printf("DEFLATE ERROR: no tree found");
                return 0;
            }
            if (!tree_root->left && !tree_root->right){
                uint8_t extra = 0;
                switch (tree_root->entry){
                    case 16:
                    READ_BITS(ctx->bytes, extra, 2, ctx->bs, ctx->c);
                    extra += 3;
                    for (int j = 0; j < extra; j++)
                        full_huffman[i+j] = last_code_len;
                    break;
                    case 17:
                    READ_BITS(ctx->bytes, extra, 3, ctx->bs, ctx->c);
                    extra += 3;
                    for (int j = 0; j < extra; j++)
                        full_huffman[i+j] = 0;
                    break;
                    case 18:
                    READ_BITS(ctx->bytes, extra, 7, ctx->bs, ctx->c);
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
        huffman_free(huff_decode_nodes);

        // printf("**** LITERAL/LENGTH ****");
        huff_tree_node *litlen_tree = deflate_decode_codes(15, hlit + 257, full_huffman);
        // huffman_viz(litlen_tree, 0, 0);

        // printf("**** DISTANCE ****");
        huff_tree_node *dist_tree = deflate_decode_codes(15, hdist + 1, full_huffman + hlit + 257);

        // huffman_viz(dist_tree, 0, 0);
        // printf("**** WOO ****");

        free(full_huffman, tree_data_size * sizeof(uint16_t));

        tree_root = litlen_tree;
        
        if (!deflate_block(litlen_tree, dist_tree, ctx)){
            huffman_free(litlen_tree);
            huffman_free(dist_tree);
            return 0;
        }
        
        huffman_free(litlen_tree);
        huffman_free(dist_tree);
    }

    // printf("Wrote a total of %i bytes",out_cursor);

    return ctx->out_cursor;
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

uint32_t paeth_predict(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t p = a + b - c;
    uint32_t pa = abs(p - a);
    uint32_t pb = abs(p - b);
    uint32_t pc = abs(p - c);

    if (pa <= pb && pa <= pc)
        return a;
    else if (pb <= pc)
        return b;
    else
        return c;
}

void png_process_raw(uintptr_t raw_img, uint32_t w, uint32_t h, uint16_t bpp, uint32_t *buf){
    const uint8_t bytes = bpp/8;
    for (uint32_t y = 0; y < h; y++){
        uint8_t filter_type = *(uint8_t*)(raw_img + (((w * bytes) + 1) * y));
        // printf("%i Filter type %i",y,filter_type);
        if (filter_type == 0){
            printf("Wrong filter type. Dumping row and returning");
            for (uint32_t x = 0; x < w; x++){
                uint32_t current = get_color_bpp(bpp, (raw_img + (((w * bytes) + 1) * y) + 1 + (x*bytes)));
                printf("%x",current);
            }
            return;
        }
        for (uint32_t x = 0; x < w; x++){
            uint32_t current = get_color_bpp(bpp, (raw_img + (((w * bytes) + 1) * y) + 1 + (x*bytes)));
            // if (y == 0) printf("%x",current);
            switch (filter_type) {
                case 0: 
                break;
                case 1:
                if (x > 0) current += buf[(y * w) + x - 1];
                    break;
                case 2:
                    if (y > 0) current += buf[((y-1) * w) + x];
                    break;
                case 3:
                    if (x > 0) current += buf[(y * w) + x - 1]/2;
                    if (y > 0) current += buf[((y-1) * w) + x]/2;
                    break;
                case 4:
                    if (x > 0 && y > 0) {
                        uint32_t prev = buf[(y * w) + x - 1];
                        uint32_t top = buf[((y - 1) * w) + x];
                        uint32_t diag = buf[((y - 1) * w) + x - 1];
                        current += paeth_predict(prev, top, diag);
                    }
                    return;
            }
            buf[(y * w) + x] = convert_bpp_color(bpp, current); 
        }
    }
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
    uint16_t bpp = 0;
    deflate_read_ctx ctx = {};
    do {
        hdr = (png_chunk_hdr*)p;
        uint32_t length = __builtin_bswap32(hdr->length);
        if (strstart(hdr->type, "IHDR", true) == 4){
            png_ihdr *ihdr = (png_ihdr*)(p + sizeof(png_chunk_hdr));
            bpp = png_decode_bpp(ihdr);
            info = (image_info){__builtin_bswap32(ihdr->width),__builtin_bswap32(ihdr->height)};
        }
        if (strstart(hdr->type, "IDAT", true) == 4){
            if (info.width == 0 || info.height == 0){
                printf("Wrong image size");
                return;
            }
            if (!out_buf){ 
                out_buf = (uintptr_t)malloc((info.width * info.height * system_bpp) + info.height);//TODO: bpp might be too big, read image format
                ctx.output_buf = (uint8_t*)out_buf;
            }
            printf("Found some idat %#x - %#x",p + sizeof(png_chunk_hdr) - (uintptr_t)file, length);
            deflate_decode((void*)(p + sizeof(png_chunk_hdr)), length, &ctx);
        }
        p += sizeof(png_chunk_hdr) + __builtin_bswap32(hdr->length) + sizeof(uint32_t);
    } while(strstart(hdr->type, "IEND", true) != 4);
    png_process_raw(out_buf, info.width, info.height, bpp, buf);
}

void* load_png(char *path, image_info *info){
    file descriptor;
    FS_RESULT res = fopen(path, &descriptor);
    void *img = 0;
    void* file_img = malloc(descriptor.size);
    fread(&descriptor, file_img, descriptor.size);
    if (res != FS_RESULT_SUCCESS){ 
        printf("Couldn't open image");
        return 0;
    }

    *info = png_get_info(file_img, descriptor.size);
    printf("info %ix%i",info->width,info->height);
    img = malloc(info->width*info->height*system_bpp);
    png_read_image(file_img, descriptor.size, img);
    return img;
}