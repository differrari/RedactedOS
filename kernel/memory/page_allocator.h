#pragma once

#include "types.h"
#include "memory_types.h"

#define ALIGN_4KB 0x1000
#define ALIGN_16B 0x10
#define ALIGN_64B 0x40
#define PAGE_SIZE 4096

#define MEM_PRIV_USER   0
#define MEM_PRIV_KERNEL 1
#define MEM_PRIV_SHARED 2

#define MEM_RW      (1 << 0)
#define MEM_RO      (0 << 0)
#define MEM_EXEC    (1 << 1)
#define MEM_DEV     (1 << 2)
#define MEM_NORM    (0 << 2)

void page_allocator_init();

#ifdef __cplusplus
extern "C" {
#endif
void page_alloc_enable_verbose();
void* palloc(uint64_t size, uint8_t level, uint8_t attributes, bool full);
void free_managed_page(void* ptr);
void pfree(void* ptr, uint64_t size);
void mark_used(uintptr_t address, size_t pages);

bool page_used(uintptr_t ptr);

void* kalloc(void *page, uint64_t size, uint16_t alignment, uint8_t level);
void kfree(void* ptr, uint64_t size);

int count_pages(uint64_t i1,uint64_t i2);

void free_sized(sizedptr ptr);

#ifdef __cplusplus
}
#endif