#pragma once

#include "types.h"
#include "memory/memory_types.h"

int memcmp(const void *s1, const void *s2, unsigned long n);
void *memset(void *dest, int val, unsigned long count);
void *memcpy(void *dest, const void *src, uint64_t n);

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

uint64_t alloc_mmio_region(uint64_t size);