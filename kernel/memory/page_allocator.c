#include "page_allocator.h"
#include "memory/talloc.h"
#include "console/serial/uart.h"
#include "mmu.h"
#include "std/memory.h"
#include "math/math.h"
#include "console/kio.h"
#include "sysregs.h"
#include "memory/addr.h"
#include "exceptions/exception_handler.h"

#define PD_TABLE 0b11
#define PD_BLOCK 0b01
#define PD_ACCESS (1 << 10)
#define BOOT_PGD_ATTR PD_TABLE
#define BOOT_PUD_ATTR PD_TABLE

#define LOW_ADDR_WARN 0x100000ULL

#define ALLOC_TAG_MAGIC 0xCCDEC00ED00DAA0EULL
#define ALLOC_TAG_MAGIC_INV (~ALLOC_TAG_MAGIC)
#define ALLOC_TAG_SIZE 64
#define ALLOC_KIND_SMALL 1

typedef struct {
    uint64_t magic;
    uint64_t magic_inv;
    uint64_t base_phys;
    uint64_t user_phys;
    uint64_t owner_phys;
    uint32_t alloc_size;
    uint32_t user_size;
    uint16_t alignment;
    uint8_t kind;
    uint8_t level;
    uint8_t attributes;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t checksum;
    uint32_t checksum_inv;
} alloc_tag;

typedef struct {
    uint64_t phys_base;
    uint64_t size;
    uint64_t owner_phys;
} big_alloc_entry;

#define BIG_ALLOC_ENTRIES ((PAGE_SIZE - 16) / sizeof(big_alloc_entry))

typedef struct big_alloc_page {
    struct big_alloc_page* next;
    uint32_t count;
    uint32_t pad;
    big_alloc_entry entries[BIG_ALLOC_ENTRIES];
} big_alloc_page;


uintptr_t *mem_bitmap;
static big_alloc_page* big_alloc_meta = 0;

static uint64_t alloc_min_page = 0;
static uint64_t alloc_max_page = 0;
static uint64_t alloc_hint_page = 0;
static uint64_t bitmap_page_count = 0;

static bool page_alloc_verbose = false;

extern uintptr_t heap_end;
static void page_alloc_init();

void page_alloc_enable_verbose(){
    page_alloc_verbose = true;
}

void page_alloc_enable_high_va(){
    uint64_t sctlr = 0;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if ((sctlr & 1) == 0) return;
    if (!mem_bitmap) return;
    if (((uintptr_t)mem_bitmap & HIGH_VA) == HIGH_VA) return;
    mem_bitmap = (uintptr_t*)PHYS_TO_VIRT((uintptr_t)mem_bitmap);
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

static inline uint64_t lowmask64(uint64_t bits) {
    if (!bits) return 0;
    if (bits >= 64) return UINT64_MAX;
    return (1ull << bits) - 1ull;
}

void pfree(void* ptr, uint64_t size) {
    if (!ptr || !size) return;
    if (!alloc_max_page) page_alloc_init();
    page_alloc_enable_high_va();
    if (!mem_bitmap || !alloc_max_page) panic("pfree init failed", (uintptr_t)ptr);

    uint64_t pages = count_pages(size,PAGE_SIZE);
    uint64_t addr = VIRT_TO_PHYS((uint64_t)ptr);
    addr /= PAGE_SIZE;
    if (addr < alloc_min_page || addr + pages > alloc_max_page) panic("pfree out of range", (uintptr_t)ptr);

    for (uint64_t i = 0; i < pages; i++){
        uint64_t index = addr + i;
        uint64_t table_index = index/64;
        uint64_t table_offset = index % 64;
        mem_bitmap[table_index] &= ~(1ULL << table_offset);
    }

    if (addr < alloc_hint_page) alloc_hint_page = addr;
    if (alloc_hint_page < alloc_min_page) alloc_hint_page = alloc_min_page;
}

void free_managed_page(void* ptr){
    if (!ptr) return;

    uintptr_t owner_phys = (uintptr_t)ptr;
    if ((owner_phys & HIGH_VA) == HIGH_VA) owner_phys = VIRT_TO_PHYS(owner_phys);
    owner_phys &= ~0xFFFULL;

    big_alloc_page* mp = big_alloc_meta;
    while (mp){
        for (uint32_t i = 0; i < mp->count;){
            if (mp->entries[i].owner_phys != owner_phys){
                i++;
                continue;
            } 

            uint64_t phys_base = mp->entries[i].phys_base;
            uint64_t size = mp->entries[i].size;

            mp->count--;
            mp->entries[i] = mp->entries[mp->count];

            pfree(PHYS_TO_VIRT_P((void*)phys_base), size);
        }
        mp = mp->next;
    }

    mem_page *info = (mem_page*)ptr;
    if (info->next)
        free_managed_page(info->next);
    pfree((void*)ptr, PAGE_SIZE);
}

static void page_alloc_init(){
    uint64_t ram_start = get_user_ram_start();
    uint64_t ram_end = get_user_ram_end();
    uint64_t start_page = ram_start / PAGE_SIZE;
    uint64_t end_page = ram_end / PAGE_SIZE;

    uint64_t words = (end_page + 63) /64;
    uint64_t bytes = words * sizeof(uint64_t);
    bitmap_page_count = count_pages(bytes, PAGE_SIZE);

    uint64_t sctlr = 0;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if ((sctlr & 1) != 0) mem_bitmap = (uintptr_t*)PHYS_TO_VIRT(ram_start);
    else mem_bitmap = (uintptr_t*)ram_start;
    memset(mem_bitmap, 0, bitmap_page_count * PAGE_SIZE);

    if (end_page & 63) {
        uint64_t tail = end_page & 63;
        mem_bitmap[words - 1] |= ~lowmask64(tail);
    }

    alloc_min_page = start_page + bitmap_page_count;
    alloc_max_page = end_page;
    alloc_hint_page = alloc_min_page;

    heap_end = alloc_min_page * PAGE_SIZE;
    mark_used(ram_start, bitmap_page_count);
}

void setup_page(uintptr_t address, uint8_t attributes){
    uint64_t sctlr = 0;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    mem_page* new_info = (mem_page*)(((sctlr & 1) != 0) ? PHYS_TO_VIRT(address) : address);
    memset(new_info, 0, sizeof(mem_page));
    new_info->next_free_mem_ptr = address + sizeof(mem_page);
    new_info->attributes = attributes;
}

paddr_t palloc_inner(uint64_t size, uint8_t level, uint8_t attributes, bool full, bool map) {
    if (!alloc_max_page) page_alloc_init();
    page_alloc_enable_high_va();
    uint64_t page_count = count_pages(size,PAGE_SIZE);
    uint64_t reg_min = alloc_min_page / 64;
    uint64_t reg_end = (alloc_max_page + 63) / 64;
    uint64_t reg_hint = alloc_hint_page / 64;

    if (page_count > 64){
        kprintfv("[page_alloc] Large allocation > 64p");
        uint64_t reg_count = page_count/64;
        uint8_t fractional = page_count % 64;
        reg_count += fractional > 0;
        uint64_t align_regs = 1;
        if (size >= GRANULE_2MB && (size & (GRANULE_2MB - 1)) == 0) align_regs = GRANULE_2MB / (PAGE_SIZE * 64);

        for (int pass = 0; pass < 2; pass++) {
        uint64_t i0 = pass == 0 ? reg_hint : reg_min;
        uint64_t i1 = pass == 0 ? reg_end : reg_hint;
        for (uint64_t i = i0; i + reg_count <= i1; i++) {
            if (align_regs > 1 && (i % align_regs) != 0) continue;
            bool found = true;
            for (uint64_t j = 0; j < reg_count; j++){
                if (fractional && j == reg_count-1)
                    found &= (mem_bitmap[i + j] & lowmask64(fractional)) == 0;
                else
                    found &= mem_bitmap[i + j] == 0;
                
                if (!found) break;
            }
            if (found){
                for (uint64_t j = 0; j < reg_count; j++){
                    if (fractional && j == reg_count-1)
                        mem_bitmap[i+j] |= lowmask64(fractional);
                    else
                        mem_bitmap[i+j] = UINT64_MAX;
                }

                alloc_hint_page = (i * 64) + page_count;
                if (alloc_hint_page < alloc_min_page) alloc_hint_page = alloc_min_page;
                mem_page* prev_page = 0;
                for (uint32_t p = 0; p < page_count; p++){
                    uintptr_t address = ((i * 64) + p) * PAGE_SIZE;
                    if (map){
                        if ((attributes & MEM_DEV) != 0 && level == MEM_PRIV_KERNEL)
                            register_device_memory(address, address);
                        else if (level != MEM_PRIV_USER)
                            register_proc_memory(address, address, attributes, level);
                        if (!full){
                            setup_page(address, attributes);

                            mem_page* curr = (mem_page*)PHYS_TO_VIRT(address);
                            if (prev_page) prev_page->next = curr;
                            prev_page = curr;

                            memset((void*)PHYS_TO_VIRT(address +sizeof(mem_page)), 0, PAGE_SIZE - sizeof(mem_page));
                        } else {
                            memset((void*)PHYS_TO_VIRT(address), 0, PAGE_SIZE);
                        }
                    }
                }
                kprintfv("[page_alloc] Final address %x", (i * 64 * PAGE_SIZE));
                return (paddr_t)(i * 64 * PAGE_SIZE);
            }
        }
        }
        return 0;
    }

    for (int pass = 0; pass < 2; pass++) {
        uint64_t i0 = pass == 0 ? reg_hint : reg_min;
        uint64_t i1 = pass == 0 ? reg_end : reg_hint;

        for (uint64_t i = i0; i < i1; i++) {
            if (mem_bitmap[i] == UINT64_MAX) continue;

            uint64_t inv = ~mem_bitmap[i];
            uint64_t bit = __builtin_ctzll(inv);
            if (bit > (64 - page_count)){ 
                continue;
            }
            while (bit < 64) {
                bool found = true;
                for (uint64_t b = bit; b < bit + page_count; b++){
                    if ((mem_bitmap[i] >> b) & 1ull){
                        bit = b + 1;
                        found = false;
                        break;
                    }
                }
                if (found) break;
            }
            if (bit >= 64) continue;
            uintptr_t first_address = 0;
            mem_page* prev_page = 0;

            for (uint64_t j = 0; j < page_count; j++){
                mem_bitmap[i] |= (1ULL << (bit + j));
                uint64_t page_index = (i * 64) + (bit + j);
                uintptr_t address = page_index * PAGE_SIZE;
                if (!first_address) first_address = address;

                if (map){
                    if ((attributes & MEM_DEV) != 0 && level == MEM_PRIV_KERNEL)
                        register_device_memory(address, address);
                    else if (level != MEM_PRIV_USER)
                        register_proc_memory(address, address, attributes, level);

                    if (!full) {
                        setup_page(address, attributes);
                        mem_page* curr = (mem_page*)PHYS_TO_VIRT(address);
                        if (prev_page) prev_page->next = curr;
                        prev_page = curr;

                        memset((void*)PHYS_TO_VIRT(address + sizeof(mem_page)), 0, PAGE_SIZE - sizeof(mem_page));
                    } else {
                        memset((void*)PHYS_TO_VIRT(address), 0, PAGE_SIZE);
                    }
                }
            }

            alloc_hint_page = (first_address / PAGE_SIZE) + page_count;
            if (alloc_hint_page < alloc_min_page) alloc_hint_page = alloc_min_page;

            kprintfv("[page_alloc] Final address %x", first_address);
            return (paddr_t)first_address;
        } 
    }

    uart_puts("[page_alloc error] Could not allocate");
    return 0;
}

void* palloc(uint64_t size, uint8_t level, uint8_t attributes, bool full){
    paddr_t phys = palloc_inner(size, level, attributes, full, true);
    if(!phys) return 0;
    return (void*)dmap_pa_to_kva(phys);
}

bool page_used(uintptr_t ptr){
    page_alloc_enable_high_va();
    if (!mem_bitmap || !alloc_max_page) return false;
    uint64_t addr = VIRT_TO_PHYS((uint64_t)ptr) / PAGE_SIZE;
    if (addr >= alloc_max_page) return false;
    uint64_t table_index = addr/64;
    uint64_t table_offset = addr % 64;
    return (mem_bitmap[table_index] >> table_offset) & 1;
}

void mark_used(uintptr_t address, size_t pages)
{
    page_alloc_enable_high_va();
    if (!mem_bitmap) return;
    address = VIRT_TO_PHYS(address);
    if ((address & (PAGE_SIZE - 1)) != 0) {
        kprintf("[mark_used error] address %x not aligned", address);
        return;
    }
    if (pages == 0) return;

    uint64_t page_index = address / PAGE_SIZE;
    for (size_t j = 0; j < pages; j++) {
        uint64_t idx = page_index + j;
        uint64_t i = idx / 64;
        uint64_t bit  = idx % 64;

        mem_bitmap[i] |= (1ULL << bit);
    }
}
void* kalloc_inner(void *page, size_t size, uint16_t alignment, uint8_t level, uintptr_t page_va, uintptr_t *next_va, uintptr_t *ttbr){
    if (!page) return 0;
    if (!size) return 0;
    if (!alignment || (alignment & (alignment - 1))) {
        kprintfv("[kalloc] bad alignment %x", alignment);
        return 0;
    }

    size_t req_size = size;
    size = (size + alignment - 1) & ~(alignment - 1);

    if ((uintptr_t)page < LOW_ADDR_WARN) kprintfv("[kalloc an] low page=%llx size=%llx align=%x", (uint64_t)(uintptr_t)page, (uint64_t)size, (uint32_t)alignment);

    if (size & 0xFULL) size = (size + 15) & ~0xFULL;

    mem_page *info = (mem_page*)PHYS_TO_VIRT_P(page);

    uintptr_t owner_phys = (uintptr_t)page;
    if ((owner_phys & HIGH_VA) == HIGH_VA) owner_phys = VIRT_TO_PHYS(owner_phys);
    owner_phys &= ~0xFFFULL;

    if (!info->next_free_mem_ptr){
        uintptr_t page_phys = (uintptr_t)page;
        if ((page_phys & HIGH_VA) == HIGH_VA) page_phys = VIRT_TO_PHYS(page_phys);
        page_phys &= ~0xFFFULL;
        setup_page(page_phys, info->attributes);//b
        info = (mem_page*)PHYS_TO_VIRT_P((void*)page_phys);
    }

    size_t small_need = ALLOC_TAG_SIZE + size + (alignment - 1);
    if (small_need & 0xFULL) small_need = (small_need + 15) & ~0xFULL;

    if (size >= PAGE_SIZE || alignment >= PAGE_SIZE || small_need >= PAGE_SIZE) {
        uint64_t alloc_size = size;
        if (alloc_size & (alignment - 1)) alloc_size = (alloc_size + alignment - 1) & ~(alignment - 1);
        alloc_size = (alloc_size + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);

        void* ptr = palloc(alloc_size, level, info->attributes, true);//b
        if (!ptr) return 0;

        uintptr_t phys_base = VIRT_TO_PHYS((uintptr_t)ptr);

        big_alloc_page* mp = big_alloc_meta;
        while (mp && mp->count >= BIG_ALLOC_ENTRIES)
            mp = mp->next;

        if (!mp) {
            paddr_t meta_phys =palloc_inner(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, true, true);
            if (!meta_phys) panic("kalloc no metadata page", alloc_size);
            mp = (big_alloc_page*)dmap_pa_to_kva(meta_phys);
            memset(mp, 0, PAGE_SIZE);
            mp->next = big_alloc_meta;
            big_alloc_meta = mp;
        }

        mp->entries[mp->count].phys_base = phys_base;
        mp->entries[mp->count].size = alloc_size;
        mp->entries[mp->count].owner_phys = owner_phys;
        mp->count++;

        if (page_va && next_va && ttbr){
            uintptr_t va = *next_va;
            for (uint64_t i = 0; i < alloc_size; i+= GRANULE_4KB){
                mmu_map_4kb((uint64_t*)ttbr, (uint64_t)*next_va, (paddr_t)(phys_base + i), (info->attributes & MEM_DEV) ? MAIR_IDX_DEVICE : MAIR_IDX_NORMAL, info->attributes, level);
                *next_va += PAGE_SIZE;
            }
            return (void*)va;
        }
        memset((void*)PHYS_TO_VIRT(phys_base), 0, alloc_size);
        return ptr;
    }

    FreeBlock** curr = PHYS_TO_VIRT_P(&info->free_list);
    if (info->free_list && (uintptr_t)info->free_list < LOW_ADDR_WARN)
        kprintfv("[kalloc an] free_list low head=%llx page=%llx", (uint64_t)(uintptr_t)info->free_list, (uint64_t)(uintptr_t)page);

    FreeBlock *cblock = PHYS_TO_VIRT_P(*curr);
    while (*curr && ((uintptr_t)*curr & 0xFFFFFFFF) != 0xDEADBEEF && (uintptr_t)cblock != 0xDEADBEEF && (uintptr_t)cblock != 0xDEADBEEFDEADBEEF) {
        uintptr_t base_phys = (uintptr_t)*curr;
        uint64_t bsz = cblock->size;

        if (bsz >= small_need) {
            uintptr_t user_phys = base_phys + ALLOC_TAG_SIZE;
            if (user_phys & (alignment - 1)) user_phys = (user_phys + alignment - 1) & ~(uintptr_t)(alignment - 1);
            uintptr_t tag_phys = user_phys - ALLOC_TAG_SIZE;

            if (user_phys + size <= base_phys + bsz) {
                *curr = cblock->next;

                alloc_tag* tag = (alloc_tag*)PHYS_TO_VIRT(tag_phys);
                tag->magic = ALLOC_TAG_MAGIC;
                tag->magic_inv = ALLOC_TAG_MAGIC_INV;
                tag->base_phys = base_phys;
                tag->user_phys = user_phys;
                tag->owner_phys = owner_phys;
                tag->alloc_size = (uint32_t)bsz;
                tag->user_size = (uint32_t)req_size;
                tag->alignment = alignment;
                tag->kind = ALLOC_KIND_SMALL;
                tag->level = level;
                tag->attributes = info->attributes;
                tag->reserved0 = 0;
                tag->reserved1 = 0;

                uint64_t mix = tag->base_phys ^ tag->user_phys ^ tag->owner_phys ^ ((uint64_t)tag->alloc_size << 32) ^ tag->user_size;
                mix ^= ((uint64_t)tag->alignment << 48) ^ ((uint64_t)tag->kind << 40) ^ ((uint64_t)tag->level << 32) ^ ((uint64_t)tag->attributes << 24);
                mix ^= ALLOC_TAG_MAGIC; //token
                uint32_t c = (uint32_t)(mix ^ (mix >> 32));
                tag->checksum = c;
                tag->checksum_inv = ~c;

                memset((void*)PHYS_TO_VIRT(user_phys), 0, size);
                info->size += bsz;
                if (page_va) return(void*)(page_va | (user_phys & 0xFFF));
                return (void*)user_phys;
            }
        }

        curr = &cblock->next;
        cblock = PHYS_TO_VIRT_P(*curr);
    }

    uintptr_t page_phys = (uintptr_t)page;
    if ((page_phys & HIGH_VA) == HIGH_VA) page_phys = VIRT_TO_PHYS(page_phys);
    page_phys &= ~0xFFFULL;

    uintptr_t base_phys = info->next_free_mem_ptr;
    if (base_phys & 0xFULL) base_phys = (base_phys + 15) & ~0xFULL;
    kprintfv("[in_page_alloc] Current next pointer %llx",info->next_free_mem_ptr);

    uintptr_t user_phys = base_phys + ALLOC_TAG_SIZE;
    if (user_phys & (alignment - 1)) user_phys = (user_phys + alignment - 1) & ~(uintptr_t)(alignment - 1);
    uintptr_t tag_phys = user_phys - ALLOC_TAG_SIZE;

    kprintfv("[in_page_alloc] Aligned next pointer %llx", base_phys);

    if (base_phys + small_need > page_phys + PAGE_SIZE) {
        uintptr_t next_page_va = page_va ? (page_va + PAGE_SIZE) : 0;
        if (next_va) next_page_va = *next_va;
        if (!info->next){
            info->next = palloc(PAGE_SIZE, level, info->attributes, false);
            if (next_va) next_page_va = *next_va;
            if (page_va && next_va && ttbr){
                uintptr_t phys_next = VIRT_TO_PHYS((uintptr_t)info->next);
                register_proc_memory((uintptr_t)*next_va, (paddr_t)phys_next, info->attributes, level);
                *next_va += PAGE_SIZE;
            }
            kprintfv("[in_page_alloc] Page %llx points to new page %llx",page,info->next);
        }
        kprintfv("[in_page_alloc] Page full. Moving to %x", (uintptr_t)info->next);
        return kalloc_inner(info->next, req_size, alignment, level, next_page_va, next_va, ttbr);
    }

    info->next_free_mem_ptr = base_phys + small_need;

    alloc_tag* tag = (alloc_tag*)PHYS_TO_VIRT(tag_phys);
    tag->magic = ALLOC_TAG_MAGIC;
    tag->magic_inv = ALLOC_TAG_MAGIC_INV;
    tag->base_phys = base_phys;
    tag->user_phys = user_phys;
    tag->owner_phys = owner_phys;
    tag->alloc_size = (uint32_t)small_need;
    tag->user_size = (uint32_t)req_size;
    tag->alignment = alignment;
    tag->kind = ALLOC_KIND_SMALL;
    tag->level = level;
    tag->attributes = info->attributes;
    tag->reserved0 = 0;
    tag->reserved1 = 0;

    uint64_t mix = tag->base_phys ^ tag->user_phys ^ tag->owner_phys ^ ((uint64_t)tag->alloc_size << 32) ^ tag->user_size;
    mix ^= ((uint64_t)tag->alignment << 48) ^ ((uint64_t)tag->kind << 40) ^ ((uint64_t)tag->level << 32) ^ ((uint64_t)tag->attributes << 24);
    mix ^= ALLOC_TAG_MAGIC;
    uint32_t c = (uint32_t)(mix ^ (mix >> 32));
    tag->checksum = c;
    tag->checksum_inv = ~c;

    memset((void*)PHYS_TO_VIRT(user_phys), 0, size);
    info->size += small_need;
    kprintfv("[in_page_alloc] Allocated address %x",user_phys);

    if (page_va){
        return (void*)(page_va | (user_phys & 0xFFF));
    }
    return (void*)user_phys;
}

void* make_page_index(){
    return palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, true);
}

void register_allocation(page_index *index, void* ptr, size_t size){
    if (!index){
        kprint("[ALLOC error] registering allocation with no index");
        return;
    }
    if (!ptr || !size){
        kprint("[ALLOC error] trying to register null allocation");
        return;
    }
    while (index->header.next) {
        index = index->header.next;
    }
    if (index->header.size >= PAGE_INDEX_LIMIT){
        index->header.next = make_page_index();
        index = index->header.next;
    }
    index->ptrs[index->header.size].ptr = ptr;
    index->ptrs[index->header.size++].size = size;
}

void free_registered(page_index *index, void *ptr){
    if (!index){
        kprint("[ALLOC error] freeing allocation with no index");
        return;
    }
    if (!ptr){
        kprint("[ALLOC error] trying to un-register null allocation");
        return;
    }
    for (page_index *ind = index; ind; ind = ind->header.next){
        for (u64 i = 0; i < ind->header.size; i++){
            if (ind->ptrs[i].ptr == ptr){
                pfree(ind->ptrs[i].ptr, ind->ptrs[i].size);
                ind->ptrs[i] = ind->ptrs[ind->header.size - 1];
                ind->header.size--;
                return;
            }
        }
    }
    kprint("[ALLOC error] trying to free non-registered page");
}

void release_page_index(page_index *index){
    if (!index){
        kprint("[ALLOC error] no page index");
        return;
    }
    if (index->header.next)
        release_page_index(index->header.next);
    for (u64 i = 0; i < index->header.size; i++){
        pfree(index->ptrs[i].ptr, index->ptrs[i].size);
    }
    pfree(index, PAGE_SIZE);
}

void* kalloc(void *page, size_t size, uint16_t alignment, uint8_t level){
    void* ptr = kalloc_inner(page, size, alignment, level, 0, 0, 0);
    if (level == MEM_PRIV_KERNEL && ptr) ptr = PHYS_TO_VIRT_P(ptr);
    return ptr;
}

void kfree(void* ptr, size_t size) {
    if(!ptr) return;
    kprintfv("[page_alloc_free] Freeing block at %x size %x",(uintptr_t)ptr, size);

    uintptr_t va = (uintptr_t)ptr;
    uintptr_t phys = 0;

    if((va & HIGH_VA) == HIGH_VA){
        phys = VIRT_TO_PHYS(va);
    } else {
        int tr = 0;
        phys = mmu_translate(0, va, &tr);
        if(tr){
            kprintf("[kfree] unmapped ptr=%llx size=%llx", (uint64_t)va, (uint64_t)size);
            panic("kfree unmapped pointer", va);
        }
    }

    uintptr_t phys_base = phys& ~0xFFFULL;
    uint64_t page_off = va & 0xFFFULL;

    bool tag_ok = false;
    alloc_tag* tag = 0;

    if(page_off >= ALLOC_TAG_SIZE) {
        uintptr_t phys_tag = 0;
        if((va & HIGH_VA) == HIGH_VA) {
            phys_tag = VIRT_TO_PHYS(va - ALLOC_TAG_SIZE);
        } else {
            int tr = 0;
            phys_tag = mmu_translate(0, va - ALLOC_TAG_SIZE, &tr);
            if(tr) phys_tag = 0;
        }

        if(phys_tag) {
            tag = (alloc_tag*)PHYS_TO_VIRT(phys_tag);
            if(tag->magic == ALLOC_TAG_MAGIC && tag->magic_inv == ALLOC_TAG_MAGIC_INV && tag->user_phys == phys){
                uint64_t mix = tag->base_phys ^ tag->user_phys ^ tag->owner_phys ^ ((uint64_t)tag->alloc_size << 32) ^ tag->user_size;
                mix ^= ((uint64_t)tag->alignment << 48) ^ ((uint64_t)tag->kind << 40) ^ ((uint64_t)tag->level << 32) ^ ((uint64_t)tag->attributes << 24);
                mix ^= ALLOC_TAG_MAGIC; //token
                uint32_t c = (uint32_t)(mix ^ (mix >> 32));
                if(tag->checksum == c && tag->checksum_inv == ~c) tag_ok = true;
            }
        }
    }

    if(tag_ok) {
        if(tag->kind != ALLOC_KIND_SMALL) {
            kprintf("[kfree] bad tag kind ptr=%llx phys=%llx kind=%u size=%llx", (uint64_t)va, (uint64_t)phys, (unsigned)tag->kind, (uint64_t)size);
            panic("kfree bad tag kind", va);
        }

        uintptr_t base_phys = (uintptr_t)tag->base_phys;
        uint64_t alloc_size = tag->alloc_size;

        if(!alloc_size || alloc_size >= PAGE_SIZE) {
            kprintf("[kfree] bad small size ptr=%llx phys=%llx base=%llx alloc=%llx", (uint64_t)va, (uint64_t)phys, (uint64_t)base_phys, alloc_size);
            panic("kfree bad small alloc size", base_phys);
        }

        uintptr_t page_phys = base_phys & ~0xFFFULL;
        mem_page *page = (mem_page *)PHYS_TO_VIRT(page_phys);

        memset32((void*)PHYS_TO_VIRT(base_phys),0xDEADBEEF,alloc_size);
        FreeBlock* block = (FreeBlock*)PHYS_TO_VIRT( base_phys);
        block->size = alloc_size;
        block->next = page->free_list;
        page->free_list = (FreeBlock*)base_phys;

        if(page->size >= alloc_size) page->size -= alloc_size;
        else page->size = 0;

        return;
    }

    if(page_off != 0) {
        kprintf("[kfree] untracked ptr=%llx phys=%llx size=%llx off=%llx", (uint64_t)va, (uint64_t)phys, (uint64_t)size, page_off);
        panic("kfree untracked pointer", va);
    }

    big_alloc_page* mp = big_alloc_meta;
    while(mp) {
        for(uint32_t i = 0; i < mp->count; i++)  {
            if(mp->entries[i].phys_base != phys_base) continue;

            if(mp->entries[i].phys_base != phys) {
                kprintf("[kfree] non base big ptr=%llx phys=%llx base=%llx", (uint64_t)va, (uint64_t)phys, (uint64_t)phys_base);
                panic("kfree non base big pointer", va);
            }

            uint64_t big_size = mp->entries[i].size;
            mp->count--;
            mp->entries[i] = mp->entries[mp->count];

            if(!mem_bitmap) {
                kprintf("[kfree] bitmap not init ptr=%llx phys=%llx", (uint64_t)va, (uint64_t)phys);
                panic("kfree bitmap not init", va);
            }

            pfree(PHYS_TO_VIRT_P((void*)phys_base), big_size);
            return;
        }
        mp = mp->next;
    }

    kprintf("[kfree] page pointer not tracked ptr=%llx phys=%llx size=%llx", (uint64_t)va, (uint64_t)phys, (uint64_t)size);
    panic("kfree page pointer not tracked (use pfree)", phys);
}

void free_sizedptr(sizedptr ptr){
    kfree((void*)ptr.ptr, ptr.size);
}