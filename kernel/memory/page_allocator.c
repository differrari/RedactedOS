#include "page_allocator.h"
#include "memory_types.h"
#include "memory_access.h"
#include "memory/kalloc.h"
#include "console/kio.h"
#include "mmu.h"
#include "interrupts/exception_handler.h"

#define PD_TABLE 0b11
#define PD_BLOCK 0b01
#define PD_ACCESS (1 << 10)
#define BOOT_PGD_ATTR PD_TABLE
#define BOOT_PUD_ATTR PD_TABLE

#define PAGE_TABLE_ENTRIES 512
#define PAGE_SIZE 4096

uint64_t mem_table_l1[PAGE_TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

static bool page_alloc_verbose = false;

void page_alloc_enable_verbose(){
    page_alloc_verbose = true;
}

#define kprintfv(fmt, ...) \
    ({ \
        if (page_alloc_verbose){\
            uint64_t _args[] = { __VA_ARGS__ }; \
            kprintf_args_raw((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
        }\
    })

void proc_map_4kb(uint64_t va, uint64_t pa) {
    uint64_t l1_index = (va >> 39) & 0x1FF;
    uint64_t l2_index = (va >> 30) & 0x1FF;
    uint64_t l3_index = (va >> 21) & 0x1FF;
    uint64_t l4_index = (va >> 12) & 0x1FF;

    if (!(mem_table_l1[l1_index] & 1)) {
        uint64_t* l2 = (uint64_t*)palloc(PAGE_SIZE);
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) l2[i] = 0;
        mem_table_l1[l1_index] = ((uint64_t)l2 & 0xFFFFFFFFF000ULL) | PD_TABLE;
    }

    uint64_t* l2 = (uint64_t*)(mem_table_l1[l1_index] & 0xFFFFFFFFF000ULL);
    if (!(l2[l2_index] & 1)) {
        uint64_t* l3 = (uint64_t*)palloc(PAGE_SIZE);
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) l3[i] = 0;
        l2[l2_index] = ((uint64_t)l3 & 0xFFFFFFFFF000ULL) | PD_TABLE;
    }

    uint64_t* l3 = (uint64_t*)(l2[l2_index] & 0xFFFFFFFFF000ULL);
    if (!(l3[l3_index] & 1)) {
        uint64_t* l4 = (uint64_t*)palloc(PAGE_SIZE);
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) l4[i] = 0;
        l3[l3_index] = ((uint64_t)l4 & 0xFFFFFFFFF000ULL) | PD_TABLE;
    }

    uint64_t* l4 = (uint64_t*)(l3[l3_index] & 0xFFFFFFFFF000ULL);
    uint64_t attr = PD_ACCESS | (1 << 2) | 0b11;
    l4[l4_index] = (pa & 0xFFFFFFFFF000ULL) | attr;
}

void page_allocator_init() {
    for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        mem_table_l1[i] = 0;
    }
}

void free_page(void* ptr, uint64_t size) {
    uint64_t addr = (uint64_t)ptr;
    size = ((size + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;

    memset((void*)addr,0,size);

    for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE) {
        uint64_t v = addr + offset;
        uint64_t l1 = (v >> 39) & 0x1FF;
        uint64_t l2 = (v >> 30) & 0x1FF;
        uint64_t l3 = (v >> 21) & 0x1FF;
        uint64_t l4 = (v >> 12) & 0x1FF;

        if (!(mem_table_l1[l1] & 1)) continue;
        uint64_t* l2t = (uint64_t*)(mem_table_l1[l1] & ~0xFFF);
        if (!(l2t[l2] & 1)) continue;
        uint64_t* l3t = (uint64_t*)(l2t[l2] & ~0xFFF);
        if (!(l3t[l3] & 1)) continue;
        uint64_t* l4t = (uint64_t*)(l3t[l3] & ~0xFFF);

        l4t[l4] = 0;

        mmu_unmap(v,v);
    }
}

typedef struct mem_page {
    struct mem_page *next;
    FreeBlock *free_list;
    uint64_t next_free_mem_ptr;
} mem_page;

void* alloc_page(uint64_t size, bool kernel, bool device, bool full) {
    uint64_t start = get_user_ram_start();
    uint64_t end = get_user_ram_end();

    start = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    end = end & ~(PAGE_SIZE - 1);

    size = ((size + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    for (uint64_t va = start; va + size <= end; va += PAGE_SIZE) {
        bool free = true;
        for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE) {
            uint64_t v = va + offset;
            uint64_t l1 = (v >> 39) & 0x1FF;
            uint64_t l2 = (v >> 30) & 0x1FF;
            uint64_t l3 = (v >> 21) & 0x1FF;
            uint64_t l4 = (v >> 12) & 0x1FF;

            if (!(mem_table_l1[l1] & 1)) continue;
            uint64_t* l2t = (uint64_t*)(mem_table_l1[l1] & ~0xFFF);
            if (!(l2t[l2] & 1)) continue;
            uint64_t* l3t = (uint64_t*)(l2t[l2] & ~0xFFF);
            if (!(l3t[l3] & 1)) continue;
            uint64_t* l4t = (uint64_t*)(l3t[l3] & ~0xFFF);
            if (l4t[l4] & 1) {
                free = false;
                break;
            }
        }
        if (free) {
            for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE){
                proc_map_4kb(va + offset, va + offset);
                if (device && kernel)
                    register_device_memory(va+offset, va+offset);
                else
                    register_proc_memory(va + offset, va + offset, kernel);
            }
            if (!full){
                mem_page* new_info = (mem_page*)va;
                new_info->next = NULL;
                new_info->free_list = NULL;
                new_info->next_free_mem_ptr = (uint64_t)va + sizeof(mem_page);
            }
            kprintf_raw("[page_alloc] Page allocated: %h", va);
            return (void*)va;
        }
    }
    kprintf_raw("Could not allocate");
    return 0;
}

void* allocate_in_page(void *page, uint64_t size, uint16_t alignment, bool kernel, bool device){
    size = (size + alignment - 1) & ~(alignment - 1);

    kprintfv("[page_alloc] Requested size: %h", size);

    mem_page *info = (mem_page*)page;

    if (size >= PAGE_SIZE){
        kprintfv("[page_alloc] Allocating full page for %h",size);
        void *first_addr = 0;
        for (int i = 0; i < size; i += PAGE_SIZE){
            void* ptr = alloc_page(PAGE_SIZE, kernel, device, true);
            memset((void*)ptr, 0, PAGE_SIZE);
            if (!first_addr) first_addr = ptr;
        }
        return first_addr;
    }

    kprintf_raw("{>>>>} %h",(uintptr_t)&info->free_list);
    FreeBlock** curr = &info->free_list;
    while (*curr) {
        if ((*curr)->size >= size) {
            kprintfv("[page_alloc] Reusing free block at %h",(uintptr_t)*curr);

            uint64_t result = (uint64_t)*curr;
            *curr = (*curr)->next;
            memset((void*)result, 0, size);
            return (void*)result;
        }
        kprintf("-> %h",(uintptr_t)&(*curr)->next);
        curr = &(*curr)->next;
    }

    kprintfv("[page_alloc] Current next pointer %h",info->next_free_mem_ptr);

    info->next_free_mem_ptr = (info->next_free_mem_ptr + alignment - 1) & ~(alignment - 1);

    kprintfv("[page_alloc] Aligned next pointer %h",info->next_free_mem_ptr);

    if (info->next_free_mem_ptr + size > (((uintptr_t)page) + PAGE_SIZE)) {
        if (!info->next)
            info->next = alloc_page(PAGE_SIZE, kernel, device, false);
        kprintfv("[page_alloc] Page full. Moving to %h",(uintptr_t)info->next);
        return allocate_in_page(info->next, size, alignment, kernel, device);
    }

    uint64_t result = info->next_free_mem_ptr;
    info->next_free_mem_ptr += size;

    kprintfv("[page_alloc] Allocated address %h",result);

    memset((void*)result, 0, size);
    return (void*)result;
}

void free_from_page(void* ptr, uint64_t size) {
    kprintfv("[page_alloc_free] Freeing block at %h size %h",(uintptr_t)ptr, size);

    memset((void*)ptr,0,size);

    mem_page *page = (mem_page *)((((uintptr_t)ptr) + 0xFFF) & ~0xFFF);

    FreeBlock* block = (FreeBlock*)ptr;
    block->size = size;
    block->next = page->free_list;
    page->free_list = block;
}