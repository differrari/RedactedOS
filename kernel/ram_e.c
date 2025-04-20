#include "ram_e.h"
#include "types.h"
#include "console/kio.h"

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

void panic(){
    printf("You're not at the disco are you? why are you panicking? We don't even have panic implemented yet");
}

#define temp_start &heap_bottom + 0x100000

extern uint64_t heap_bottom;
extern uint64_t heap_limit;
uint64_t next_free_temp_memory = (uint64_t)&heap_bottom;
uint64_t next_free_perm_memory = (uint64_t)temp_start;

uint64_t talloc(uint64_t size) {
    next_free_temp_memory = (next_free_temp_memory + 0xFFF) & ~0xFFF;
    if (next_free_temp_memory + size > next_free_perm_memory)
        panic("Temporary allocator overflow");
    uint64_t result = next_free_temp_memory;
    next_free_temp_memory += (size + 0xFFF) & ~0xFFF;
    return result;
}

uint64_t palloc(uint64_t size) {
    next_free_perm_memory = (next_free_perm_memory + 0xFFF) & ~0xFFF;
    if (next_free_perm_memory > heap_limit)
        panic("Permanent allocator overflow");
    uint64_t result = next_free_perm_memory;
    next_free_perm_memory += (size + 0xFFF) & ~0xFFF;
    return result;
}

void free_temp(){
    next_free_temp_memory = (uint64_t)temp_start;
}