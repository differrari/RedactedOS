#include "talloc.h"
#include "types.h"
#include "exceptions/exception_handler.h"
#include "console/kio.h"
#include "hw/hw.h"
#include "dtb.h"
#include "console/serial/uart.h"
#include "memory/memory_access.h"
#include "std/string.h"
#include "std/memfunctions.h"

static uint64_t total_ram_size = 0;
static uint64_t total_ram_start = 0;
static uint64_t calculated_ram_size = 0;
static uint64_t calculated_ram_start = 0;
static uint64_t calculated_ram_end = 0;

FreeBlock* temp_free_list = 0;

#define PCI_MMIO_LIMIT  MMIO_BASE + 0xFFFFFFF

static uint64_t next_mmio_base;

//TODO: come up with a fix for hardcoded MMIO for some devices
uint64_t alloc_mmio_region(uint64_t size) {
    if (next_mmio_base == 0) next_mmio_base = MMIO_BASE;
    size = (size + 0xFFF) & ~0xFFF;
    if (next_mmio_base + size > PCI_MMIO_LIMIT){
        panic_with_info("MMIO alloc overflow",next_mmio_base+size);
        return 0;
    }
    uint64_t addr = next_mmio_base;
    next_mmio_base += size;
    return addr;
}

bool is_mmio_allocated(uint64_t addr){
    return addr > MMIO_BASE && addr < next_mmio_base;
}

extern uint64_t kernel_start;
extern uint64_t heap_bottom;
extern uint64_t heap_limit;
extern uint64_t kcode_end;
extern uint64_t kfull_end;
extern uint64_t shared_start;
extern uint64_t shared_end;
static bool talloc_verbose = false;

uint64_t next_free_temp_memory = (uint64_t)&heap_bottom;

uint64_t talloc(uint64_t size) {

    size = (size + 0xFFF) & ~0xFFF;

    if (talloc_verbose){
        uart_raw_puts("[talloc] Requested size: ");
        uart_puthex(size);
        uart_raw_putc('\n');
    }

    FreeBlock** curr = &temp_free_list;
    while (*curr) {
        if ((*curr)->size >= size) {
            if (talloc_verbose){
                uart_raw_puts("[talloc] Reusing free block at ");
                uart_puthex((uint64_t)*curr);
                uart_raw_putc('\n');
            }

            uint64_t result = (uint64_t)*curr;
            *curr = (*curr)->next;
            memset((void*)result, 0, size);
            return result;
        }
        curr = &(*curr)->next;
    }

    if (next_free_temp_memory + size > (uintptr_t)&heap_limit) {
        panic_with_info("Kernel allocator overflow", next_free_temp_memory);
    }

    uint64_t result = next_free_temp_memory;
    next_free_temp_memory += size;

    if (talloc_verbose){
        uart_raw_puts("[talloc] Allocated address ");
        uart_puthex(result);
        uart_raw_putc('\n');
    }

    memset((void*)result, 0, size);
    return result;
}

void temp_free(void* ptr, uint64_t size) {
    size = (size + 0xFFF) & ~0xFFF;
    if (talloc_verbose){
        uart_raw_puts("[temp_free] Freeing block at ");
        uart_puthex((uint64_t)ptr);
        uart_raw_puts(" size ");
        uart_puthex(size);
        uart_raw_putc('\n');
    }

    memset((void*)ptr,0,size);

    FreeBlock* block = (FreeBlock*)ptr;
    block->size = size;
    block->next = temp_free_list;
    temp_free_list = block;
}

void enable_talloc_verbose(){
    talloc_verbose = true;
}

uint64_t mem_get_kmem_start(){
    return (uint64_t)&kernel_start;
}

uint64_t mem_get_kmem_end(){
    return (uint64_t)&kcode_end;
}

int handle_mem_node(const char *propname, const void *prop, uint32_t len, dtb_match_t *match) {
    if (strcmp(propname, "reg", false) == 0 && len >= 16) {
        uint32_t *p = (uint32_t *)prop;
        match->reg_base = ((uint64_t)__builtin_bswap32(p[0]) << 32) | __builtin_bswap32(p[1]);
        match->reg_size = ((uint64_t)__builtin_bswap32(p[2]) << 32) | __builtin_bswap32(p[3]);
        
        return 1;
    }
    if (strcmp(propname, "device_type", false) == 0 && strcmp(prop,"memory", false) == 0){
        match->found = true;
    }
    return 0;
}

int get_memory_region(uint64_t *out_base, uint64_t *out_size) {
    dtb_match_t match = {0};
    if (dtb_scan("memory",handle_mem_node, &match)) {
        *out_base = match.reg_base;
        *out_size = match.reg_size;
        return 1;
    }
    return 0;
}

void calc_ram(){
    if (get_memory_region(&total_ram_start, &total_ram_size)) {
            calculated_ram_end = total_ram_start + total_ram_size;
            calculated_ram_start = ((uint64_t)&kfull_end) + 0x1;
            calculated_ram_start = ((calculated_ram_start) & ~((1ULL << 21) - 1));
            calculated_ram_end = ((calculated_ram_end) & ~((1ULL << 21) - 1));
    } else {
        total_ram_start = RAM_START;
        total_ram_size = RAM_SIZE;
        calculated_ram_end = CRAM_END;
        calculated_ram_start = CRAM_START;
    }
    calculated_ram_size = calculated_ram_end - calculated_ram_start;
}

#define calcvar(var)\
    if (var == 0)\
        calc_ram();\
    return var;

uint64_t get_total_ram(){
    calcvar(total_ram_size)
}

uint64_t get_total_user_ram(){
    calcvar(calculated_ram_size)
}

uint64_t get_user_ram_start(){
    calcvar(calculated_ram_start)
}

uint64_t get_user_ram_end(){
    calcvar(calculated_ram_end)
}

uint64_t get_shared_start(){
    return (uint64_t)&shared_start;
}

uint64_t get_shared_end(){
    return (uint64_t)&shared_end;
}