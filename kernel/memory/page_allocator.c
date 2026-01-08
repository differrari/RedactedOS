#include "page_allocator.h"
#include "memory/talloc.h"
#include "console/serial/uart.h"
#include "mmu.h"
#include "exceptions/exception_handler.h"
#include "std/memory.h"
#include "math/math.h"
#include "console/kio.h"
#include "process/scheduler.h"

#define PD_TABLE 0b11
#define PD_BLOCK 0b01
#define PD_ACCESS (1 << 10)
#define BOOT_PGD_ATTR PD_TABLE
#define BOOT_PUD_ATTR PD_TABLE

#define PAGE_TABLE_ENTRIES 65536

uint64_t mem_bitmap[PAGE_TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

static bool page_alloc_verbose = false;

uint64_t start;
uint64_t end;

void page_alloc_enable_verbose(){
    page_alloc_verbose = true;
}

#define kprintfv(fmt, ...) \
    ({ \
        if (page_alloc_verbose){\
            kprintf(fmt, ##__VA_ARGS__); \
        }\
    })

void page_allocator_init() {
    memset(mem_bitmap, 0, sizeof(mem_bitmap));
}

uint64_t count_pages(uint64_t i1,uint64_t i2){
    return (i1/i2) + (i1 % i2 > 0);
}

void pfree(void* ptr, uint64_t size) {
    uint64_t  pages = count_pages(size,PAGE_SIZE);
    uint64_t addr = (uint64_t)ptr;
    addr /= PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++){
        uint64_t index = addr + i;
        uint64_t table_index = index/64;
        uint64_t table_offset = index % 64;
        mem_bitmap[table_index] &= ~(1ULL << table_offset);
        mmu_unmap(index * PAGE_SIZE,index * PAGE_SIZE);
    }
    if (addr < start) start = addr;
}

void* palloc(uint64_t size, uint8_t level, uint8_t attributes, bool full) {
    if (!size) return 0;

    if (!start) start = count_pages(get_user_ram_start(),PAGE_SIZE);
    if (!end) end = count_pages(get_user_ram_end(),PAGE_SIZE);
    uint64_t page_count = count_pages(size,PAGE_SIZE);

    if (page_count > 64){
        kprintfv("Large allocation > 64p");
        uint64_t reg_count = page_count/64;
        uint64_t fractional = page_count % 64;
        reg_count += (fractional > 0);

        uint64_t i_begin = (start + 63) / 64;

        for (uint64_t i = i_begin; i < end/64; i++) {
            bool found = true;
            for (uint64_t j = 0; j < reg_count; j++){
                if (fractional && j == reg_count-1)
                    found &= (mem_bitmap[i + j] & ((1ULL << fractional) - 1)) == 0;
                else
                    found &= mem_bitmap[i + j] == 0;

                if (!found) break;
            }
            if (!found) continue;

            for (uint64_t j = 0; j < reg_count; j++){
                if (fractional && j == reg_count-1)
                    mem_bitmap[i+j] |= ((1ULL << fractional) - 1);
                else
                    mem_bitmap[i+j] = UINT64_MAX;
            }

            uintptr_t addr = (i * 64) * PAGE_SIZE;

            for (uint64_t p = 0; p < page_count; p++){
                uintptr_t address = addr + (p * PAGE_SIZE);

                if ((attributes & MEM_DEV) && (level == MEM_PRIV_KERNEL))
                    register_device_memory(address, address);
                else
                    register_proc_memory(address, address, attributes, level);

                if (!full) {
                    mem_page *new_info = (mem_page*)address;
                    new_info->next = ((p + 1) < page_count) ? (mem_page*)(address + PAGE_SIZE) : NULL;
                    new_info->free_list = NULL;
                    new_info->next_free_mem_ptr = address + sizeof(mem_page);
                    new_info->attributes = attributes;
                    new_info->size = 0;
                }
            }

            uint64_t next = (i * 64) + page_count;
            if (next > start) start = next;
            if (full) memset((void*)addr, 0, size);
            return (void*)addr;
        }
    }

    bool skipped_regs = false;
    uint64_t end_reg = (end + 63) / 64;
    for (uint64_t i = start/64; i < end_reg; i++) {
        if (mem_bitmap[i] == UINT64_MAX) {
            kprintfv("Normal allocation");
            if (!skipped_regs) start = (i + 1) * 64;
            continue;
        }

        uint64_t b_begin = 0;

        if (i == (start / 64)) b_begin = start % 64;

        uint64_t run = 0;
        uint64_t run_start = 0;

        for (uint64_t b = b_begin; b < 64; b++){
            if (((mem_bitmap[i] >> b) & 1) == 0){
                if (!run) run_start = b;
                run++;
                if (run == page_count) break;
            } else run = 0;
        }

        if (run < page_count){
            skipped_regs = true;
            continue;
        }

        uintptr_t first_address = 0;

        for (uint64_t j = 0; j < page_count; j++){
            mem_bitmap[i] |= (1ULL << (run_start + j));
            uint64_t page_index = (i * 64) + run_start + j;
            uintptr_t address = page_index * PAGE_SIZE;
            if (!first_address) first_address = address;

                if ((attributes & MEM_DEV) != 0 && level == MEM_PRIV_KERNEL)
                    register_device_memory(address, address);
                else
                    register_proc_memory(address, address, attributes, level);

            if (!full) {
                mem_page* new_info = (mem_page*)address;

                new_info->next = ((j + 1) < page_count) ? (mem_page*)(address + PAGE_SIZE) : NULL;
                new_info->free_list = NULL;
                new_info->next_free_mem_ptr = address + sizeof(mem_page);
                new_info->attributes = attributes;
                new_info->size = 0;
            }
        }

        kprintfv("[page_alloc] Final address %x", first_address);

        start = (i * 64) + run_start + page_count;

        if (full) memset((void*)first_address,0,size);
        return (void*)first_address;
    }

    uart_puts("[page_alloc error] Could not allocate");
    return 0;
}

bool page_used(uintptr_t ptr){
    uint64_t addr = (uint64_t)ptr;
    addr /= PAGE_SIZE;
    uint64_t table_index = addr/64;
    uint64_t table_offset = addr % 64;
    return (mem_bitmap[table_index] >> table_offset) & 1;
}

void mark_used(uintptr_t address, size_t pages)
{
    if ((address & (PAGE_SIZE - 1)) != 0) {
        kprintf("[mark_used error] address %x not aligned", address);
        return;
    }
    if (!pages) return;

    uint64_t first_page = address / PAGE_SIZE;

    for (size_t j = 0; j < pages; j++) {
        uint64_t page = first_page + j;
        uint64_t i = page / 64;
        uint64_t bit = page % 64;

        mem_bitmap[i] |= (1ULL << bit);
    }
}

//TODO: maybe alloc to different base pages based on alignment? Then it's easier to keep track of full pages, freeing and sizes
void* kalloc(void *page, uint64_t size, uint16_t alignment, uint8_t level){
    //TODO: we're changing the size but not reporting it back, which means the free function does not fully free the allocd memory
    if (!alignment || (alignment & (alignment - 1))) alignment = 16;

    if (size < sizeof(FreeBlock)) size = sizeof(FreeBlock);
    
    size = (size + alignment - 1) & ~(alignment - 1);

    kprintfv("[in_page_alloc] Requested size: %x", size);

    mem_page *info = (mem_page*)page;

    if (info->next_free_mem_ptr == 0 || info->next_free_mem_ptr < (uintptr_t)page || info->next_free_mem_ptr > ((uintptr_t)page + PAGE_SIZE)) {
        info->next = NULL;
        info->free_list = NULL;
        info->next_free_mem_ptr = (uintptr_t)page + sizeof(mem_page);
        info->attributes = MEM_RW | MEM_NORM;
        info->size = 0;
    }

    if (size >= PAGE_SIZE){
        void* ptr = palloc(size, level, info->attributes, true);
        return ptr;
    }

    FreeBlock** curr = &info->free_list;
    while (*curr) {
        if ((*curr)->size >= size) {
            kprintfv("[in_page_alloc] Reusing free block at %x",(uintptr_t)*curr);

            uint64_t result = (uint64_t)*curr;
            *curr = (*curr)->next;
            memset((void*)result, 0, size);
            info->size += size;
            return (void*)result;
        }
        kprintfv("-> %x",(uintptr_t)&(*curr)->next);
        curr = &(*curr)->next;
    }

    kprintfv("[in_page_alloc] Current next pointer %x",info->next_free_mem_ptr);

    info->next_free_mem_ptr = (info->next_free_mem_ptr + alignment - 1) & ~(alignment - 1);

    kprintfv("[in_page_alloc] Aligned next pointer %x",info->next_free_mem_ptr);

    if (info->next_free_mem_ptr + size > (((uintptr_t)page) + PAGE_SIZE)) {
        if (!info->next)
            info->next = palloc(PAGE_SIZE, level, info->attributes, false);
        kprintfv("[in_page_alloc] Page full. Moving to %x",(uintptr_t)info->next);
        return kalloc(info->next, size, alignment, level);
    }

    uint64_t result = info->next_free_mem_ptr;
    info->next_free_mem_ptr += size;

    kprintfv("[in_page_alloc] Allocated address %x",result);

    memset((void*)result, 0, size);
    info->size += size;
    return (void*)result;
}

void kfree(void* ptr, uint64_t size) {
    kprintfv("[page_alloc_free] Freeing block at %x size %x",(uintptr_t)ptr, size);

    if (!ptr || !size) return;

    if (size < sizeof(FreeBlock)) size = sizeof(FreeBlock);
    if ((((uintptr_t)ptr & (PAGE_SIZE - 1)) == 0) && (size >= PAGE_SIZE)){
        pfree(ptr,size);
        return;
    }

    memset((void*)ptr,0,size);

    mem_page *page = (mem_page *)(((uintptr_t)ptr) & ~(PAGE_SIZE - 1));

    if (!page_used((uintptr_t)page))return;


    FreeBlock* block = (FreeBlock*)ptr;
    block->size = size;

    FreeBlock **curr = &page->free_list;
    while (*curr && *curr < block) curr = &(*curr)->next;

    block->next = *curr;
    *curr = block;

    if (block->next && ((uintptr_t)block + block->size == (uintptr_t)block->next)) {
        block->size += block->next->size;
        block->next = block->next->next;
    }

    if (curr != &page->free_list) {
        FreeBlock *prev = page->free_list;
        while (prev && prev->next != block) prev = prev->next;

        if (prev && ((uintptr_t)prev + prev->size == (uintptr_t)block)) {
            prev->size += block->size;
            prev->next = block->next;
        }
    }

    if (page->size >= size) page->size -= size;
    else page->size = 0;
}

void free_sized(sizedptr ptr){
    kfree((void*)ptr.ptr, ptr.size);
}