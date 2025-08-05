#pragma once

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

#ifdef __cplusplus
}
#endif