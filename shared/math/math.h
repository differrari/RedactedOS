#pragma once

#include "rng.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline int min(int a, int b){
    return a < b ? a : b;
}

static inline int max(int a, int b){
    return a > b ? a : b;
}

static inline int abs(int n){
    return n < 0 ? -n : n;
}

static inline int sign(int x) {
    return x < 0 ? -1 : 1;
}

static inline int lerp(int i, int start, int end, int steps) {
    return start + (end - start) * i / steps;
}

static inline int ceil(float val){
    uint64_t whole = (uint64_t)val;
    double frac = val - (double)whole;

    return frac > 0 ? val + 1 : val;
}

#ifdef __cplusplus
}
#endif