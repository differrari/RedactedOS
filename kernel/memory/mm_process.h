#pragma once

#include "types.h"
#include "memory/page_allocator.h"

typedef struct process process_t;

#define VMA_FLAG_DEMAND 1
#define VMA_FLAG_USERALLOC 2
#define VMA_FLAG_ZERO 4
#define VMA_FLAG_NOFREE 8
#define VMA_KIND_ELF 1
#define VMA_KIND_HEAP 2
#define VMA_KIND_STACK 3
#define VMA_KIND_ANON 4
#define VMA_KIND_SPECIAL 5

#define MAX_VMAS 128
#define MM_GAP_PAGES 16

typedef struct vma {
    uaddr_t start;
    uaddr_t end;
    uint8_t prot;
    uint8_t kind;
    uint8_t flags;
} vma;

typedef struct mm_free_range {
    uaddr_t start;
    uaddr_t end;
} mm_free_range;

typedef struct mm_struct {
    uintptr_t *ttbr0;
    paddr_t ttbr0_phys;
    uint16_t asid;
    uint32_t asid_gen;
    vma vmas[MAX_VMAS];
    uint16_t vma_count;
    mm_free_range mmap_free[MAX_VMAS];
    uint16_t mmap_free_count;
    uaddr_t heap_start;
    uaddr_t brk;
    uaddr_t brk_max;
    uaddr_t mmap_top;
    uaddr_t mmap_cursor;
    uaddr_t stack_top;
    uaddr_t stack_limit;
    uaddr_t stack_commit;
    uint64_t rss_heap_pages;
    uint64_t rss_stack_pages;
    uint64_t rss_anon_pages;
    uint64_t cap_heap_pages;
    uint64_t cap_stack_pages;
    uint64_t cap_anon_pages;
} mm_struct;

vma* mm_find_vma(mm_struct *mm, uaddr_t va);
bool mm_add_vma(mm_struct *mm, uaddr_t start, uaddr_t end, uint8_t prot, uint8_t kind, uint8_t flags);
bool mm_remove_vma(mm_struct *mm, uaddr_t start, uaddr_t end);
uaddr_t mm_alloc_mmap(mm_struct *mm, size_t size, uint8_t prot, uint8_t kind, uint8_t flags);
bool mm_try_handle_page_fault(process_t *proc, uintptr_t far, uint64_t esr);