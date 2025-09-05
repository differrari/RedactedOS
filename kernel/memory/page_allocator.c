#include "page_allocator.h"
#include "memory_access.h"
#include "memory/talloc.h"
#include "console/serial/uart.h"
#include "mmu.h"
#include "exceptions/exception_handler.h"
#include "std/memory.h"
#include "math/math.h"

#define PD_TABLE 0b11
#define PD_BLOCK 0b01
#define PD_ACCESS (1 << 10)
#define BOOT_PGD_ATTR PD_TABLE
#define BOOT_PUD_ATTR PD_TABLE

#define PAGE_TABLE_ENTRIES 65536

uint64_t mem_bitmap[PAGE_TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

static bool page_alloc_verbose = false;

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
    for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        mem_bitmap[i] = 0;
    }
}

void pfree(void* ptr, uint64_t size) {
    //TODO: review this, we're not using size
    uint64_t addr = (uint64_t)ptr;
    addr /= PAGE_SIZE;
    uint64_t table_index = addr/64;
    uint64_t table_offset = addr % 64;
    mem_bitmap[table_index] &= ~(1ULL << table_offset);
}

int count_pages(uint64_t i1,uint64_t i2){
    return (i1/i2) + (i1 % i2 > 0);
}

uint64_t start;
uint64_t end;

//TODO: prepare for allocating more than 64 bits by marking full registers at a time
void* palloc(uint64_t size, uint8_t level, uint8_t attributes, bool full) {
    if (!start) start = count_pages(get_user_ram_start(),PAGE_SIZE);
    if (!end) end = count_pages(get_user_ram_end(),PAGE_SIZE);
    uint64_t page_count = count_pages(size,PAGE_SIZE);

    if (page_count > 64){
        uint64_t reg_count = page_count/64;
        uint8_t fractional = page_count % 64;
        reg_count += fractional > 0;
        
        for (uint64_t i = start/64; i < end/64; i++) {
            bool found = true;
            for (uint64_t j = 0; j < reg_count; j++){
                if (fractional && j == reg_count-1)
                    found &= (mem_bitmap[i + j] & ((1ULL << (fractional + 1)) - 1)) == 0;
                else
                    found &= mem_bitmap[i + j] == 0;
                
                if (!found) break;
            }
            if (found){
                for (uint64_t j = 0; j < reg_count; j++){
                    if (fractional && j == reg_count-1)
                        mem_bitmap[i+j] |= ((1ULL << (fractional + 1)) - 1);
                    else
                        mem_bitmap[i+j] = UINT64_MAX;
                }
                
                start = (i + (reg_count - (fractional > 0))) * 64;
                for (uint32_t p = 0; p < page_count; p++){
                    uintptr_t address = ((i * 64) + p) * PAGE_SIZE;
                    if ((attributes & MEM_DEV) != 0 && level == MEM_PRIV_KERNEL)
                        register_device_memory(address, address);
                    else
                        register_proc_memory(address, address, attributes, level);
                }
                return (void*)(i * 64 * PAGE_SIZE);
            }
        }
    }

    bool skipped_regs = false;

    for (uint64_t i = start/64; i < end/64; i++) {
        if (mem_bitmap[i] != UINT64_MAX) {
            uint64_t inv = ~mem_bitmap[i];
            uint64_t bit = __builtin_ctzll(inv);
            if (bit > (64 - page_count)){ 
                skipped_regs = true;
                continue;
            }
            do {
                bool found = true;
                for (uint64_t b = bit; b < (uint64_t)min(64,bit + (page_count - 1)); b++){
                    if (((mem_bitmap[i] >> b) & 1)){
                        bit += page_count;
                        found = false;
                    }
                }
                if (found) break;
            } while (bit < 64);
            if (bit == 64){ 
                skipped_regs = true;
                continue;
            }
            
            uintptr_t first_address = 0;
            for (uint64_t j = 0; j < page_count; j++){
                mem_bitmap[i] |= (1ULL << (bit + j));
                uint64_t page_index = (i * 64) + (bit + j);
                uintptr_t address = page_index * PAGE_SIZE;
                if (!first_address) first_address = address;

                if ((attributes & MEM_DEV) != 0 && level == MEM_PRIV_KERNEL)
                    register_device_memory(address, address);
                else
                    register_proc_memory(address, address, attributes, level);

                if (!full) {
                    mem_page* new_info = (mem_page*)address;
                    new_info->next = NULL;
                    new_info->free_list = NULL;
                    new_info->next_free_mem_ptr = address + sizeof(mem_page);
                    new_info->attributes = attributes;
                    new_info->size = 0;
                }
            }

            // kprintfv("[page_alloc] Final address %x", first_address);

            return (void*)first_address;
        } else if (!skipped_regs) start = (i + 1) * 64;
    }

    uart_puts("[page_alloc error] Could not allocate");
    return 0;
}

void mark_used(uintptr_t address, size_t pages)
{
    if ((address & (PAGE_SIZE - 1)) != 0) {
        // kprintf("[mark_used error] address %x not aligned", address);
        return;
    }
    if (pages == 0) return;

    uint64_t start = count_pages(get_user_ram_start(),PAGE_SIZE);

    uint64_t page_index = (address / (PAGE_SIZE * 64)) - (start/64);

    for (size_t j = 0; j < pages; j++) {
        uint64_t idx = page_index + j;
        uint64_t i = idx / 64;
        uint64_t bit  = idx % 64;

        mem_bitmap[i] |= (1ULL << bit);
    }
}

void* kalloc(void *page, uint64_t size, uint16_t alignment, uint8_t level){
    //TODO: we're changing the size but not reporting it back, which means the free function does not fully free the allocd memory
    if (size > UINT32_MAX)//TODO: This serves to catch an issue, except if we put this if in, the issue does not happen
        panic("Faulty allocation", size);
    
    size = (size + alignment - 1) & ~(alignment - 1);

    // kprintfv("[in_page_alloc] Requested size: %x", size);

    mem_page *info = (mem_page*)page;

    if (size >= PAGE_SIZE){
        void* ptr = palloc(size, level, info->attributes, true);
        memset((void*)ptr, 0, size);
        return ptr;
    }

    FreeBlock** curr = &info->free_list;
    while (*curr) {
        if ((*curr)->size >= size) {
            // kprintfv("[in_page_alloc] Reusing free block at %x",(uintptr_t)*curr);

            uint64_t result = (uint64_t)*curr;
            *curr = (*curr)->next;
            memset((void*)result, 0, size);
            info->size += size;
            return (void*)result;
        }
        // kprintfv("-> %x",(uintptr_t)&(*curr)->next);
        curr = &(*curr)->next;
    }

    // kprintfv("[in_page_alloc] Current next pointer %x",info->next_free_mem_ptr);

    info->next_free_mem_ptr = (info->next_free_mem_ptr + alignment - 1) & ~(alignment - 1);

    // kprintfv("[in_page_alloc] Aligned next pointer %x",info->next_free_mem_ptr);

    if (info->next_free_mem_ptr + size > (((uintptr_t)page) + PAGE_SIZE)) {
        if (!info->next)
            info->next = palloc(PAGE_SIZE, level, info->attributes, false);
        // kprintfv("[in_page_alloc] Page full. Moving to %x",(uintptr_t)info->next);
        return kalloc(info->next, size, alignment, level);
    }

    uint64_t result = info->next_free_mem_ptr;
    info->next_free_mem_ptr += size;

    // kprintfv("[in_page_alloc] Allocated address %x",result);

    memset((void*)result, 0, size);
    info->size += size;
    return (void*)result;
}

void kfree(void* ptr, uint64_t size) {
    // kprintfv("[page_alloc_free] Freeing block at %x size %x",(uintptr_t)ptr, size);

    memset((void*)ptr,0,size);

    mem_page *page = (mem_page *)(((uintptr_t)ptr) & ~0xFFF);

    FreeBlock* block = (FreeBlock*)ptr;
    block->size = size;
    block->next = page->free_list;
    page->free_list = block;
    page->size -= size;
}

void free_sized(sizedptr ptr){
    kfree((void*)ptr.ptr, ptr.size);
}