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

static inline int ceil(float val){
    uint64_t whole = (uint64_t)val;
    double frac = val - (double)whole;

    return frac > 0 ? val + 1 : val;
}

static inline int floor(float val){
    return (uint64_t)val;
}

#ifdef __cplusplus
}
#endif