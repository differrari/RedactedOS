#pragma once

#include "types.h"

typedef struct {
    uint8_t *bytes;
    uint8_t bs;
    int c;
    uintptr_t out_cursor;
    uint8_t *output_buf;
    bool final_block;
    size_t cur_block;
} deflate_read_ctx;

size_t deflate_decode(void* ptr, size_t size, deflate_read_ctx *ctx);