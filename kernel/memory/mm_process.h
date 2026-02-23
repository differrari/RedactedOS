#pragma once

#include "types.h"
#include "memory/page_allocator.h"

#define VMA_FLAG_DEMAND 1
#define VMA_KIND_ELF 1
#define VMA_KIND_HEAP 2
#define VMA_KIND_STACK 3
#define VMA_KIND_ANON 4

#define MAX_VMAS 32
#define MM_GAP_PAGES 16

typedef struct vma {
    uintptr_t start;
    uintptr_t end;
    uint8_t prot;
    uint8_t kind;
    uint8_t flags;
} vma;

typedef struct mm_struct {
    uintptr_t *ttbr0;
    vma vmas[MAX_VMAS];
    uint16_t vma_count;
    uintptr_t heap_start;
    uintptr_t brk;
    uintptr_t brk_max;
    uintptr_t mmap_top;
    uintptr_t mmap_cursor;
    uintptr_t stack_top;
    uintptr_t stack_limit;
    uintptr_t stack_commit;
    uint64_t rss_heap_pages;
    uint64_t rss_stack_pages;
    uint64_t rss_anon_pages;
    uint64_t cap_heap_pages;
    uint64_t cap_stack_pages;
    uint64_t cap_anon_pages;
} mm_struct;

vma* mm_find_vma(mm_struct *mm, uintptr_t va);
bool mm_add_vma(mm_struct *mm, uintptr_t start, uintptr_t end, uint8_t prot, uint8_t kind, uint8_t flags);
bool mm_update_vma(mm_struct *mm, uintptr_t start, uintptr_t end);
uintptr_t mm_alloc_mmap(mm_struct *mm, size_t size, uint8_t prot, uint8_t kind, uint8_t flags);