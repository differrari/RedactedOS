#pragma once

typedef uint32_t color;

typedef struct {
    uint32_t x;
    uint32_t y;
}__attribute__((packed)) point;

typedef struct {
    uint32_t width;
    uint32_t height;
}__attribute__((packed)) size;

typedef struct {
    point point;
    size size;
}__attribute__((packed)) rect;
