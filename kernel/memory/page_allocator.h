#pragma once

#include "types.h"

#define ALIGN_4KB 0x1000
#define ALIGN_16B 0x10
#define ALIGN_64B 0x40

void page_alloc_enable_verbose();

//TODO: move to memory class when we finally refactor ram_e, since this is not exclusive for mems
void* alloc_page(uint64_t size, bool kernel, bool device, bool full);//TODO: rename to reflect paging
void free_page(void* ptr, uint64_t size);//TODO: rename to reflect paging

void* allocate_in_page(void *page, uint64_t size, uint16_t alignment, bool kernel, bool device);
void free_from_page(void* ptr, uint64_t size);

//Used only for mmu init
uintptr_t get_paging_start();
uintptr_t get_paging_end();