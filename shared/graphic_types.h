#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t color;

typedef struct {
    uint32_t x;
    uint32_t y;
}__attribute__((packed)) gpu_point;

typedef struct {
    uint32_t width;
    uint32_t height;
}__attribute__((packed)) gpu_size;

typedef struct {
    gpu_point point;
    gpu_size size;
}__attribute__((packed)) gpu_rect;

#ifdef __cplusplus
}
#endif