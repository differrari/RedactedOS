#pragma once

#include "rng.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef INFINITY
#define INFINITY __builtin_inff()
#endif

static inline int min(int a, int b){
    return a < b ? a : b;
}

static inline float minf(float a, float b){
    return a < b ? a : b;
}

static inline int max(int a, int b){
    return a > b ? a : b;
}

static inline float maxf(float a, float b){
    return a > b ? a : b;
}

static inline int abs(int n){
    return n < 0 ? -n : n;
}

static inline float absf(float n){
    return n < 0 ? -n : n;
}

static inline float clampf(float v, float min, float max){
    float t = v < min ? min : v;
    return t > max ? max : t;
}

static inline int sign(int x) {
    return x < 0 ? -1 : 1;
}

static inline int signf(float x) {
    return x < 0 ? -1 : 1;
}

static inline int lerp(int i, int start, int end, int steps) {
    return start + (end - start) * i / steps;
}

static inline bool float_zero(float a){
    const float epsilon = 1e-6f;
    return absf(a) < epsilon;
}

static inline float lerpf(float a, float b, float t) {
  // Exact, monotonic, bounded, determinate, and (for a=b=0) consistent:
  if((a<=0 && b>=0) || (a>=0 && b<=0)) return t*b + (1-t)*a;

  if(t==1) return b;                        // exact
  // Exact at t=0, monotonic except near t=1,
  // bounded, determinate, and consistent:
  const float x = a + t*(b-a);
  return (t>1) == (b>a) ? max(b,x) : min(b,x);  // monotonic near t=1
}

static inline double ceil(double val){
    uint64_t whole = (uint64_t)val;
    double frac = val - (double)whole;

    return frac > 0 ? whole + 1 : whole;
}

static inline double floor(double val){
    return (uint64_t)val;
}

static inline int64_t abs_i64(int64_t v){
    return v < 0 ? -v : v;
}

static inline int64_t clamp_i64(int64_t v, int64_t lo, int64_t hi){
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint64_t sqrt_u64(uint64_t x){
    uint64_t op = x;
    uint64_t res = 0;
    uint64_t one = 1ULL << 62;
    while (one > op) one >>= 2;
    while (one != 0) {
        if (op >= res + one) {
            op -= res + one;
            res = (res >> 1) + one;
        } else {
            res >>= 1;
        }
        one >>= 2;
    }
    return res;
}

#ifdef __cplusplus
}
#endif