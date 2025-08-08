#include "dma.h"
#include "memory/talloc.h"

void dma_read(void* dest, uint32_t size, uint64_t pointer) {
    uint8_t* d = (uint8_t*)dest;
    for (uint32_t i = 0; i < size; i++) {
        d[i] = *(volatile uint8_t*)(uintptr_t)(pointer + i);
    }
}