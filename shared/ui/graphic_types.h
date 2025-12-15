#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t color;

typedef union argbcolor {
    struct {
        uint32_t blue: 8;
        uint32_t green: 8;
        uint32_t red: 8;
        uint32_t alpha: 8;
    };
    uint32_t color;
} argbcolor;

typedef struct {
    uint32_t x;
    uint32_t y;
} gpu_point;

typedef struct {
    int32_t x;
    int32_t y;
} int_point;

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