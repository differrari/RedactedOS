#pragma once

#include "types.h"

uint8_t read8(uintptr_t addr);
void write8(uintptr_t addr, uint8_t value);
uint16_t read16(uintptr_t addr);
void write16(uintptr_t addr, uint16_t value);
uint32_t read32(uintptr_t addr);
void write32(uintptr_t addr, uint32_t value);
uint64_t read64(uintptr_t addr);
void write64(uintptr_t addr, uint64_t value);
void write(uint64_t addr, uint64_t value);
uint64_t read(uint64_t addr);
void write_barrier();

int memcmp(const void *s1, const void *s2, unsigned long n);

#ifdef __cplusplus
extern "C" {
#endif
uint64_t talloc(uint64_t size);
void temp_free(void* ptr, uint64_t size);
void enable_talloc_verbose();
#ifdef __cplusplus
}
#endif

uint64_t palloc(uint64_t size);


uint64_t mem_get_kmem_start();
uint64_t mem_get_kmem_end();
uint64_t get_total_ram();
uint64_t get_total_user_ram();
uint64_t get_user_ram_start();
uint64_t get_user_ram_end();
uint64_t get_shared_start();
uint64_t get_shared_end();