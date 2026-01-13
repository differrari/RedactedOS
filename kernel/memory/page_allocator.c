#include "page_allocator.h"
#include "memory/talloc.h"
#include "console/serial/uart.h"
#include "mmu.h"
#include "exceptions/exception_handler.h"
#include "std/memory.h"
#include "math/math.h"
#include "console/kio.h"
#include "process/scheduler.h"
#include "sysregs.h"

#define PD_TABLE 0b11
#define PD_BLOCK 0b01
#define PD_ACCESS (1 << 10)
#define BOOT_PGD_ATTR PD_TABLE
#define BOOT_PUD_ATTR PD_TABLE

#define PAGE_TABLE_ENTRIES 65536

uintptr_t *mem_bitmap;

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

uint64_t count_pages(uint64_t i1,uint64_t i2){
    return (i1/i2) + (i1 % i2 > 0);
}

void pfree(void* ptr, uint64_t size) {
    int pages = count_pages(size,PAGE_SIZE);
    uint64_t addr = VIRT_TO_PHYS((uint64_t)ptr);
    addr /= PAGE_SIZE;
    for (int i = 0; i < pages; i++){
        uint64_t index = addr + i;
        uint64_t table_index = index/64;
        uint64_t table_offset = index % 64;
        mem_bitmap[table_index] &= ~(1ULL << table_offset);
        mmu_unmap(index * PAGE_SIZE, index * PAGE_SIZE);
    }
}

void free_page_list(page_index *index){
    if (index->header.next) free_page_list(index->header.next);
    for (size_t i = 0; i < index->header.size; i++)
        pfree(index->ptrs[i].ptr, index->ptrs[i].size);
}

void free_managed_page(void* ptr){
    mem_page *info = (mem_page*)ptr;
    if (info->next)
        free_managed_page(info->next);
    if (info->page_alloc)
        free_page_list(info->page_alloc);
    pfree((void*)ptr, PAGE_SIZE);
}

uint64_t start;
uint64_t end;

void setup_page(uintptr_t address, uint8_t attributes){
    mem_page* new_info = (mem_page*)address;
    memset(new_info, 0, sizeof(mem_page));
    new_info->next_free_mem_ptr = address + sizeof(mem_page);
    new_info->attributes = attributes;
}

extern uintptr_t heap_end;

void* palloc_inner(uint64_t size, uint8_t level, uint8_t attributes, bool full, bool map) {
    if (!start || !end) {
        start = count_pages(get_user_ram_start(),PAGE_SIZE); 
        end = count_pages(get_user_ram_end(),PAGE_SIZE);
        mem_bitmap = (uintptr_t*)(start * PAGE_SIZE);
        start += count_pages(65536*8,PAGE_SIZE);
        heap_end = start*PAGE_SIZE;
    }
    uint64_t page_count = count_pages(size,PAGE_SIZE);

    if (page_count > 64){
        kprintfv("[page_alloc] Large allocation > 64p");
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
                    if (map){
                        if ((attributes & MEM_DEV) != 0 && level == MEM_PRIV_KERNEL)
                            register_device_memory(address, address);
                        else
                            register_proc_memory(address, address, attributes, level);
                    }
                }
                kprintfv("[page_alloc] Final address %x", (i * 64 * PAGE_SIZE));
                void* addr = (void*)(i * 64 * PAGE_SIZE);
                if (map) memset(PHYS_TO_VIRT_P(addr), 0, size);
                return addr;
            }
        }
    }

    bool skipped_regs = false;

    for (uint64_t i = start/64; i < end/64; i++) {
        if (mem_bitmap[i] != UINT64_MAX) {
            kprintfv("Normal allocation");
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

                if (map){
                    if ((attributes & MEM_DEV) != 0 && level == MEM_PRIV_KERNEL)
                        register_device_memory(address, address);
                    else
                        register_proc_memory(address, address, attributes, level);
                }

                if (!full && map) {
                    setup_page(address, attributes);
                }
            }

            kprintfv("[page_alloc] Final address %x", first_address);
            if (map){
                size_t extra = full ? 0 : sizeof(mem_page);
                memset((void*)PHYS_TO_VIRT((first_address + extra)),0,size - extra);
            } 
            return (void*)first_address;
        } else if (!skipped_regs) start = (i + 1) * 64;
    }

    uart_puts("[page_alloc error] Could not allocate");
    return 0;
}

void* palloc(uint64_t size, uint8_t level, uint8_t attributes, bool full){
    void* phys = palloc_inner(size, level, attributes, full, true);
    if(!phys) return 0;
    return PHYS_TO_VIRT_P(phys);
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

#define PAGE_INDEX_LIMIT (PAGE_SIZE-sizeof(page_index_hdr))/sizeof(page_index_entry)

//TODO: maybe alloc to different base pages based on alignment? Then it's easier to keep track of full pages, freeing and sizes
void* kalloc_inner(void *page, size_t size, uint16_t alignment, uint8_t level, uintptr_t page_va, uintptr_t *next_va, uintptr_t *ttbr){
    //TODO: we're changing the size but not reporting it back, which means the free function does not fully free the allocd memory
    if (!page) return 0;
    size = (size + alignment - 1) & ~(alignment - 1);

    kprintfv("[in_page_alloc] Requested size: %x", size);

    mem_page *info = (mem_page*)PHYS_TO_VIRT_P(page);
    if (!info->next_free_mem_ptr){
        uintptr_t page_phys = (uintptr_t)page;
        if ((page_phys & HIGH_VA) == HIGH_VA) page_phys = VIRT_TO_PHYS(page_phys);
        setup_page(page_phys, info->attributes);

        info = (mem_page*)PHYS_TO_VIRT_P((void*)page_phys);
    }
    
    if (size >= PAGE_SIZE){
        void* ptr = palloc(size, level, info->attributes, true);
        page_index *index = info->page_alloc;
        if (!index){
            info->page_alloc = palloc(PAGE_SIZE, level, info->attributes, true);
            index = info->page_alloc;
        }
        while (index->header.next) {
            index = index->header.next;
        }
        if (index->header.size >= PAGE_INDEX_LIMIT){
            index->header.next = palloc(PAGE_SIZE, level, info->attributes, true);
            index = index->header.next;
        }
        index->ptrs[index->header.size].ptr = ptr;
        index->ptrs[index->header.size++].size = size;
        if (page_va && next_va && ttbr){
            uintptr_t va = *next_va;
            for (uintptr_t i = (uintptr_t)ptr; i < (uintptr_t)ptr + size; i+= GRANULE_4KB){
                mmu_map_4kb(ttbr, *next_va, (uintptr_t)i, (info->attributes & MEM_DEV) ? MAIR_IDX_DEVICE : MAIR_IDX_NORMAL, info->attributes, level);
                *next_va += PAGE_SIZE;
            }
            return (void*)va;
        }
        return ptr;
    }

    FreeBlock** curr = PHYS_TO_VIRT_P(&info->free_list);
    FreeBlock *cblock = PHYS_TO_VIRT_P(*curr);
    while (*curr && ((uintptr_t)*curr & 0xFFFFFFFF) != 0xDEADBEEF && (uintptr_t)cblock != 0xDEADBEEF && (uintptr_t)cblock != 0xDEADBEEFDEADBEEF) {
        if (cblock->size >= size) {
            kprintfv("[in_page_alloc] Reusing free block at %x",(uintptr_t)*curr);

            uint64_t result = (uint64_t)cblock;
            //*curr = VIRT_TO_PHYS_P(cblock->next);
            *curr = cblock->next;
            memset((void*)PHYS_TO_VIRT(result), 0, size);
            info->size += size;
            if (page_va){
                return (void*)(page_va | (result & 0xFFF));
            } 
            return (void*)result;
        }
        kprintfv("-> %x",(uintptr_t)&cblock->next);
        curr = &(cblock)->next;
        cblock = PHYS_TO_VIRT_P(*curr);
    }

    kprintfv("[in_page_alloc] Current next pointer %llx",info->next_free_mem_ptr);

    info->next_free_mem_ptr = (info->next_free_mem_ptr + alignment - 1) & ~(alignment - 1);

    kprintfv("[in_page_alloc] Aligned next pointer %llx",info->next_free_mem_ptr);

    if (info->next_free_mem_ptr + size > ((VIRT_TO_PHYS((uintptr_t)page)) + PAGE_SIZE)) {
        if (!info->next){
            info->next = palloc(PAGE_SIZE, level, info->attributes, false);
            if (page_va && next_va && ttbr){
                mmu_map_4kb(ttbr, *next_va, (uintptr_t)info->next, (info->attributes & MEM_DEV) ? MAIR_IDX_DEVICE : MAIR_IDX_NORMAL, info->attributes, level);
                *next_va += PAGE_SIZE;
            }
            kprintfv("[in_page_alloc] Page %llx points to new page %llx",page,info->next);
        }
        kprintfv("[in_page_alloc] Page full. Moving to %x",(uintptr_t)info->next);
        return kalloc_inner(info->next, size, alignment, level, page_va ? page_va + PAGE_SIZE : 0, next_va, ttbr);
    }

    uint64_t result = info->next_free_mem_ptr;
    info->next_free_mem_ptr += size;

    kprintfv("[in_page_alloc] Allocated address %x",result);

    memset((void*)PHYS_TO_VIRT(result), 0, size);
    info->size += size;
    if (page_va){
        return (void*)(page_va | (result & 0xFFF));
    }
    return (void*)result;
}

//TODO: rather than kalloc, it should be palloc that does translations
void* kalloc(void *page, size_t size, uint16_t alignment, uint8_t level){
    void* ptr = kalloc_inner(page, size, alignment, level, 0, 0, 0);
    if (level == MEM_PRIV_KERNEL) ptr = PHYS_TO_VIRT_P(ptr);
    return ptr;
}

void kfree(void* ptr, size_t size) {
    if(!ptr || size == 0) return;
    kprintfv("[page_alloc_free] Freeing block at %x size %x",(uintptr_t)ptr, size);

    if(size & 0xF) size = (size + 15) & ~0xFULL;

    memset32((void*)ptr,0xDEADBEEF,size);

    mem_page *page = (mem_page *)(((uintptr_t)ptr) & ~0xFFFULL);
    uintptr_t phys_page = mmu_translate((uintptr_t)page);
    uintptr_t off = (uintptr_t)ptr & 0xFFFULL;
    uintptr_t block_phys = phys_page | off;

    FreeBlock* block = (FreeBlock*)PHYS_TO_VIRT(block_phys);
    block->size = size;
    block->next = page->free_list;
    page->free_list = (FreeBlock*)block_phys;
    page->size -= size;
}

void free_sizedptr(sizedptr ptr){
    kfree((void*)ptr.ptr, ptr.size);
}