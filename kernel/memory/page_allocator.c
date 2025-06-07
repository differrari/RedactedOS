#include "page_allocator.h"
#include "memory_access.h"
#include "memory/kalloc.h"
#include "console/kio.h"
#include "mmu.h"
#include "interrupts/exception_handler.h"
#include "std/memfunctions.h"

#define PD_TABLE 0b11
#define PD_BLOCK 0b01
#define PD_ACCESS (1 << 10)
#define BOOT_PGD_ATTR PD_TABLE
#define BOOT_PUD_ATTR PD_TABLE

#define PAGE_TABLE_ENTRIES 65536
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

int count_pages(uint64_t i1,uint64_t i2){
    return (i1/i2) + (i1 % i2 > 0);
}

void* alloc_page(uint64_t size, bool kernel, bool device, bool full) {
    uint64_t start = count_pages(get_user_ram_start(),PAGE_SIZE);
    uint64_t end = count_pages(get_user_ram_end(),PAGE_SIZE);
    uint64_t page_count = count_pages(size,PAGE_SIZE);

    for (uint64_t i = start/64; i < end/64; i++) {
        if (mem_table_l1[i] != UINT64_MAX) {
            uint64_t inv = ~mem_table_l1[i];
            uint64_t bit = __builtin_ctzll(inv);
            do {
                bool found = true;
                for (uint64_t b = bit; b < bit + (page_count - 1); b++){
                    if (!mem_table_l1[i] >> b & 1){
                        bit += page_count;
                        found = false;
                    }
                }
                if (found) break;
            } while (bit < 64);
            if (bit == 64) continue;
            
            uintptr_t first_address = 0;
            for (uint64_t j = 0; j < page_count; j++){
                mem_table_l1[i] |= (1ULL << bit + j);
                uint64_t page_index = (i * 64) + (bit + j);
                uintptr_t address = page_index * PAGE_SIZE;
                if (!first_address) first_address = address;

                if (device && kernel)
                    register_device_memory(address, address);
                else
                    register_proc_memory(address, address, kernel);

                if (!full) {
                    mem_page* new_info = (mem_page*)address;
                    new_info->next = NULL;
                    new_info->free_list = NULL;
                    new_info->next_free_mem_ptr = address + sizeof(mem_page);
                    new_info->size = 0;
                }
            }

            kprintfv("[page_alloc] Final address %h", first_address);

            return (void*)first_address;
        }
    }

    kprintf_raw("[page_alloc error] Could not allocate");
    return 0;
}

void* allocate_in_page(void *page, uint64_t size, uint16_t alignment, bool kernel, bool device){
    size = (size + alignment - 1) & ~(alignment - 1);

    kprintfv("[in_page_alloc] Requested size: %h", size);

    mem_page *info = (mem_page*)page;

    if (size >= PAGE_SIZE){
        kprintfv("[page_alloc] Allocating full page for %h",size);
        void *first_addr = 0;
        for (uint64_t i = 0; i < size; i += PAGE_SIZE){
            void* ptr = alloc_page(PAGE_SIZE, kernel, device, true);
            memset((void*)ptr, 0, PAGE_SIZE);
            if (!first_addr) first_addr = ptr;
        }
        //TODO: we're not keeping track of this size
        return first_addr;
    }

    FreeBlock** curr = &info->free_list;
    while (*curr) {
        if ((*curr)->size >= size) {
            kprintfv("[in_page_alloc] Reusing free block at %h",(uintptr_t)*curr);

            uint64_t result = (uint64_t)*curr;
            *curr = (*curr)->next;
            memset((void*)result, 0, size);
            info->size += size;
            return (void*)result;
        }
        kprintfv("-> %h",(uintptr_t)&(*curr)->next);
        curr = &(*curr)->next;
    }

    kprintfv("[in_page_alloc] Current next pointer %h",info->next_free_mem_ptr);

    info->next_free_mem_ptr = (info->next_free_mem_ptr + alignment - 1) & ~(alignment - 1);

    kprintfv("[in_page_alloc] Aligned next pointer %h",info->next_free_mem_ptr);

    if (info->next_free_mem_ptr + size > (((uintptr_t)page) + PAGE_SIZE)) {
        if (!info->next)
            info->next = alloc_page(PAGE_SIZE, kernel, device, false);
        kprintfv("[in_page_alloc] Page full. Moving to %h",(uintptr_t)info->next);
        return allocate_in_page(info->next, size, alignment, kernel, device);
    }

    uint64_t result = info->next_free_mem_ptr;
    info->next_free_mem_ptr += size;

    kprintfv("[in_page_alloc] Allocated address %h",result);

    memset((void*)result, 0, size);
    info->size += size;
    return (void*)result;
}

void free_from_page(void* ptr, uint64_t size) {
    kprintfv("[page_alloc_free] Freeing block at %h size %h",(uintptr_t)ptr, size);

    memset((void*)ptr,0,size);

    mem_page *page = (mem_page *)(((uintptr_t)ptr) & ~0xFFF);

    FreeBlock* block = (FreeBlock*)ptr;
    block->size = size;
    block->next = page->free_list;
    page->free_list = block;
    page->size -= size;
}