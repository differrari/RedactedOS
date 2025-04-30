#include "ram_e.h"
#include "types.h"
#include "exception_handler.h"
#include "console/kio.h"
#include "dtb.h"
#include "console/serial/uart.h"
#include "kstring.h"

static uint64_t total_ram_size = 0;
static uint64_t total_ram_start = 0;
static uint64_t calculated_ram_size = 0;
static uint64_t calculated_ram_start = 0;
static uint64_t calculated_ram_end = 0;

typedef struct FreeBlock {
    struct FreeBlock* next;
    uint64_t size;
} FreeBlock;

FreeBlock* temp_free_list = 0;

uint8_t read8(uintptr_t addr) {
    return *(volatile uint8_t*)addr;
}

void write8(uintptr_t addr, uint8_t value) {
    *(volatile uint8_t*)addr = value;
}

uint16_t read16(uintptr_t addr) {
    return *(volatile uint16_t*)addr;
}

void write16(uintptr_t addr, uint16_t value) {
    *(volatile uint16_t*)addr = value;
}

uint32_t read32(uintptr_t addr) {
    return *(volatile uint32_t*)addr;
}

void write32(uintptr_t addr, uint32_t value) {
    *(volatile uint32_t*)addr = value;
}

uint64_t read64(uintptr_t addr) {
    return *(volatile uint64_t*)addr;
}

void write64(uintptr_t addr, uint64_t value) {
    *(volatile uint64_t*)addr = value;
}

void write(uint64_t addr, uint64_t value) {
    write64(addr, value);
}

uint64_t read(uint64_t addr) {
    return read64(addr);
}

int memcmp(const void *s1, const void *s2, unsigned long n) {
    const unsigned char *a = s1;
    const unsigned char *b = s2;
    for (unsigned long i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] - b[i];
    }
    return 0;
}

void *memset(void *dest, int val, unsigned long count) {
    unsigned char *ptr = dest;
    while (count--) {
        *ptr++ = (unsigned char)val;
    }
    return dest;
}

#define temp_start (uint64_t)&heap_bottom + 0x500000

extern uint64_t kernel_start;
extern uint64_t heap_bottom;
extern uint64_t heap_limit;
extern uint64_t kcode_end;
extern uint64_t kfull_end;
extern uint64_t shared_start;
extern uint64_t shared_end;
uint64_t next_free_temp_memory = (uint64_t)&heap_bottom;
uint64_t next_free_perm_memory = temp_start;

//We'll need to use a table indicating which sections of memory are available
//So we can talloc and free dynamically

static bool talloc_verbose = false;

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
            return result;
        }
        curr = &(*curr)->next;
    }

    if (next_free_temp_memory + size > temp_start) {
        panic_with_info("Temporary allocator overflow", next_free_temp_memory);
    }

    uint64_t result = next_free_temp_memory;
    next_free_temp_memory += size;

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

    FreeBlock* block = (FreeBlock*)ptr;
    block->size = size;
    block->next = temp_free_list;
    temp_free_list = block;
}

void enable_talloc_verbose(){
    talloc_verbose = true;
}

uint64_t palloc(uint64_t size) {
    uint64_t aligned_size = (size + 0xFFF) & ~0xFFF;
    next_free_perm_memory = (next_free_perm_memory + 0xFFF) & ~0xFFF;
    if (next_free_perm_memory + aligned_size > (uint64_t)&heap_limit)
        panic_with_info("Permanent allocator overflow", (uint64_t)&heap_limit);
    uint64_t result = next_free_perm_memory;
    next_free_perm_memory += aligned_size;
    return result;
}

uint64_t mem_get_kmem_start(){
    return (uint64_t)&kernel_start;
}

uint64_t mem_get_kmem_end(){
    return (uint64_t)&kcode_end;
}

int handle_mem_node(const char *name, const char *propname, const void *prop, uint32_t len, dtb_match_t *match) {
    if (strcmp(propname, "reg") == 0 && len >= 16) {
        uint32_t *p = (uint32_t *)prop;
        match->reg_base = ((uint64_t)__builtin_bswap32(p[0]) << 32) | __builtin_bswap32(p[1]);
        match->reg_size = ((uint64_t)__builtin_bswap32(p[2]) << 32) | __builtin_bswap32(p[3]);
        
        return 1;
    }
    if (strcmp(propname, "device_type") == 0, strcmp(prop,"memory") == 0){
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
        
        calculated_ram_size = calculated_ram_end - calculated_ram_start;
        kprintf("Device has %h memory starting at %h. %h for user starting at %h  ",total_ram_size, total_ram_start, calculated_ram_size, calculated_ram_start);
    }
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