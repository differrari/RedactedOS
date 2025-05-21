#pragma once

#include "types.h"

typedef struct FreeBlock {
    uint64_t size;
    struct FreeBlock* next;
} FreeBlock;

typedef struct mem_page {
    struct mem_page *next;
    FreeBlock *free_list;
    uint64_t next_free_mem_ptr;
    uint64_t size;
} mem_page;