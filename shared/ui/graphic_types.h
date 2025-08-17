#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t color;

typedef struct {
    uint32_t x;
    uint32_t y;
} gpu_point;

typedef struct {
    uint32_t width;
    uint32_t height;
} gpu_size;

typedef struct {
    gpu_point point;
    gpu_size size;
} gpu_rect;

#ifdef __cplusplus
}
#endif