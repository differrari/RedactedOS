#pragma once

#include "types.h"

typedef struct FreeBlock {
    uint64_t size;
    struct FreeBlock* next;
} FreeBlock;

typedef struct {
    size_t size;
    struct page_index *next;
} page_index_hdr;

typedef struct {
    void *ptr;
    size_t size;
} page_index_entry;

typedef struct page_index {
    page_index_hdr header;
    page_index_entry ptrs[];
} page_index;

typedef struct mem_page {
    struct mem_page *next;
    page_index* page_alloc;
    FreeBlock *free_list;
    uint64_t next_free_mem_ptr;
    uint64_t size;
    uint8_t attributes;
} mem_page;