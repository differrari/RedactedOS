#pragma once

#ifndef asm
#define asm __asm__
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Note: only include freestanding headers here
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sizedptr {
    uintptr_t ptr;
    size_t size;
} sizedptr;

#ifdef __cplusplus
} // extern "C"
#endif
