#include "image.h"
#include "std/memory_access.h"

uint32_t convert_color_bpp(uint16_t bpp, uintptr_t value_ptr){
    switch (bpp) {
        case 1: return 0;

        case 4: return 0;

        case 8: return 0;

        case 24: return ARGB(0xFF, read8(value_ptr + 2), read8(value_ptr + 1), read8(value_ptr)); 

        case 32: return value_ptr % 8 == 0 ? *(uint32_t*)value_ptr : read_unaligned32((uint32_t*)value_ptr);
    }
    return 0;
}