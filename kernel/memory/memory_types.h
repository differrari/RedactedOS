#pragma once

#include "types.h"
#include "alloc/page_index.h"

//DEADLINE: 01/04/2026 - will merge with alloc/mem_types.h

typedef struct FreeBlock {
    uint64_t size;
    struct FreeBlock* next;
} FreeBlock;

typedef struct mem_page {
    struct mem_page *next;
    page_index* page_alloc;
    FreeBlock *free_list;
    uint64_t next_free_mem_ptr;
    uint64_t size;
    uint8_t attributes;
} mem_page;