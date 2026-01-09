#pragma once
#include "types.h"
#include "math.h"
#if defined(__x86_64__)
#include "ammintrin.h"
#endif

typedef int8_t int8x8_t __attribute__((vector_size(8)));
typedef int8_t int8x16_t __attribute__((vector_size(16)));
typedef uint8_t uint8x8_t __attribute__((vector_size(8)));
typedef uint8_t uint8x16_t __attribute__((vector_size(16)));
typedef int16_t int16x4_t __attribute__((vector_size(8)));
typedef int16_t int16x8_t __attribute__((vector_size(16)));
typedef uint16_t uint16x4_t __attribute__((vector_size(8)));
typedef uint16_t uint16x8_t __attribute__((vector_size(16)));
typedef int32_t int32x2_t __attribute__((vector_size(8)));
typedef int32_t int32x4_t __attribute__((vector_size(16)));
typedef uint32_t uint32x2_t __attribute__((vector_size(8)));
typedef uint32_t uint32x4_t __attribute__((vector_size(16)));
typedef int64_t int64x1_t __attribute__((vector_size(8)));
typedef int64_t int64x2_t __attribute__((vector_size(16)));
typedef uint64_t uint64x1_t __attribute__((vector_size(8)));
typedef uint64_t uint64x2_t __attribute__((vector_size(16)));
typedef float float32x2_t __attribute__((vector_size(8)));
typedef float float32x4_t __attribute__((vector_size(16)));
typedef uint8_t poly8x8_t __attribute__((vector_size(8)));
typedef uint8_t poly8x16_t __attribute__((vector_size(16)));
typedef uint16_t poly16x4_t __attribute__((vector_size(8)));
typedef uint16_t poly16x8_t __attribute__((vector_size(16)));
typedef uint64_t poly64x1_t __attribute__((vector_size(8)));
typedef uint64_t poly64x2_t __attribute__((vector_size(16)));

typedef struct { int8x8_t val[2]; } int8x8x2_t;
typedef struct { int8x8_t val[3]; } int8x8x3_t;
typedef struct { int8x8_t val[4]; } int8x8x4_t;
typedef struct { int8x16_t val[2]; } int8x16x2_t;
typedef struct { int8x16_t val[3]; } int8x16x3_t;
typedef struct { int8x16_t val[4]; } int8x16x4_t;
typedef struct { uint8x8_t val[2]; } uint8x8x2_t;
typedef struct { uint8x8_t val[3]; } uint8x8x3_t;
typedef struct { uint8x8_t val[4]; } uint8x8x4_t;
typedef struct { uint8x16_t val[2]; } uint8x16x2_t;
typedef struct { uint8x16_t val[3]; } uint8x16x3_t;
typedef struct { uint8x16_t val[4]; } uint8x16x4_t;
typedef struct { int16x4_t val[2]; } int16x4x2_t;
typedef struct { int16x4_t val[3]; } int16x4x3_t;
typedef struct { int16x4_t val[4]; } int16x4x4_t;
typedef struct { int16x8_t val[2]; } int16x8x2_t;
typedef struct { int16x8_t val[3]; } int16x8x3_t;
typedef struct { int16x8_t val[4]; } int16x8x4_t;
typedef struct { uint16x4_t val[2]; } uint16x4x2_t;
typedef struct { uint16x4_t val[3]; } uint16x4x3_t;
typedef struct { uint16x4_t val[4]; } uint16x4x4_t;
typedef struct { uint16x8_t val[2]; } uint16x8x2_t;
typedef struct { uint16x8_t val[3]; } uint16x8x3_t;
typedef struct { uint16x8_t val[4]; } uint16x8x4_t;
typedef struct { int32x2_t val[2]; } int32x2x2_t;
typedef struct { int32x2_t val[3]; } int32x2x3_t;
typedef struct { int32x2_t val[4]; } int32x2x4_t;
typedef struct { int32x4_t val[2]; } int32x4x2_t;
typedef struct { int32x4_t val[3]; } int32x4x3_t;
typedef struct { int32x4_t val[4]; } int32x4x4_t;
typedef struct { uint32x2_t val[2]; } uint32x2x2_t;
typedef struct { uint32x2_t val[3]; } uint32x2x3_t;
typedef struct { uint32x2_t val[4]; } uint32x2x4_t;
typedef struct { uint32x4_t val[2]; } uint32x4x2_t;
typedef struct { uint32x4_t val[3]; } uint32x4x3_t;
typedef struct { uint32x4_t val[4]; } uint32x4x4_t;
typedef struct { int64x1_t val[2]; } int64x1x2_t;
typedef struct { int64x1_t val[3]; } int64x1x3_t;
typedef struct { int64x1_t val[4]; } int64x1x4_t;
typedef struct { int64x2_t val[2]; } int64x2x2_t;
typedef struct { int64x2_t val[3]; } int64x2x3_t;
typedef struct { int64x2_t val[4]; } int64x2x4_t;
typedef struct { uint64x1_t val[2]; } uint64x1x2_t;
typedef struct { uint64x1_t val[3]; } uint64x1x3_t;
typedef struct { uint64x1_t val[4]; } uint64x1x4_t;
typedef struct { uint64x2_t val[2]; } uint64x2x2_t;
typedef struct { uint64x2_t val[3]; } uint64x2x3_t;
typedef struct { uint64x2_t val[4]; } uint64x2x4_t;
typedef struct { float32x2_t val[2]; } float32x2x2_t;
typedef struct { float32x2_t val[3]; } float32x2x3_t;
typedef struct { float32x2_t val[4]; } float32x2x4_t;
typedef struct { float32x4_t val[2]; } float32x4x2_t;
typedef struct { float32x4_t val[3]; } float32x4x3_t;
typedef struct { float32x4_t val[4]; } float32x4x4_t;
typedef struct { poly8x8_t val[2]; } poly8x8x2_t;
typedef struct { poly8x8_t val[3]; } poly8x8x3_t;
typedef struct { poly8x8_t val[4]; } poly8x8x4_t;
typedef struct { poly8x16_t val[2]; } poly8x16x2_t;
typedef struct { poly8x16_t val[3]; } poly8x16x3_t;
typedef struct { poly8x16_t val[4]; } poly8x16x4_t;
typedef struct { poly16x4_t val[2]; } poly16x4x2_t;
typedef struct { poly16x4_t val[3]; } poly16x4x3_t;
typedef struct { poly16x4_t val[4]; } poly16x4x4_t;
typedef struct { poly16x8_t val[2]; } poly16x8x2_t;
typedef struct { poly16x8_t val[3]; } poly16x8x3_t;
typedef struct { poly16x8_t val[4]; } poly16x8x4_t;
typedef struct { poly64x1_t val[2]; } poly64x1x2_t;
typedef struct { poly64x1_t val[3]; } poly64x1x3_t;
typedef struct { poly64x1_t val[4]; } poly64x1x4_t;
typedef struct { poly64x2_t val[2]; } poly64x2x2_t;
typedef struct { poly64x2_t val[3]; } poly64x2x3_t;
typedef struct { poly64x2_t val[4]; } poly64x2x4_t;

static __inline__ __attribute__((always_inline)) float32x2_t vld1_f32_b(const float* p) { return *(const float32x2_t*)p; }
static __inline__ __attribute__((always_inline)) void vst1_f32_b(float* p, float32x2_t v) { *(float32x2_t*)p = v; }
static __inline__ __attribute__((always_inline)) float32x2_t vmul_f32_b(float32x2_t a, float32x2_t b) { return a * b; }

static __inline__ __attribute__((always_inline)) float32x2_t vpadd_f32_b(float32x2_t a, float32x2_t b){
#if defined(__APPLE__)
  return (float32x2_t)__builtin_neon_vpadd_v((int8x8_t)a, (int8x8_t)b, 9);
#elif defined(__aarch64__)
  return __builtin_aarch64_faddpv2sf(a, b);
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  return (float32x2_t)__builtin_neon_vpaddv2sf(a, b);
#elif defined(__x86_64__)
  return (float32x2_t){a[0] + b[0], a[1] + b[1]};
#else
# error "no vpadd_f32_b builtin"
#endif
}

static __inline__ __attribute__((always_inline)) float32x2_t vmax_f32_b(float32x2_t a, float32x2_t b){
#if defined(__APPLE__)
  return (float32x2_t)__builtin_neon_vmax_v((int8x8_t)a, (int8x8_t)b, 9);
#elif defined(__aarch64__)
  return __builtin_aarch64_fmax_nanv2sf(a, b);
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  return (float32x2_t)__builtin_neon_vmaxfv2sf(a, b);
#elif defined(__x86_64__)
  return a;//TODO: not implemented
#else
# error "no vmax_f32_b builtin"
#endif
}

static __inline__ __attribute__((always_inline)) float32x2_t vdup_n_f32_b(float x){
#if defined(__APPLE__)
  return (float32x2_t){ x, x };
#elif defined(__aarch64__)
  return (float32x2_t){ x, x };
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  return (float32x2_t)__builtin_neon_vdup_nv2sf(x);
#elif defined(__x86_64__)
  return (float32x2_t){ x, x };
#else
# error "no vdup_n_f32_b builtin"
#endif
}

static __inline__ __attribute__((always_inline)) float32x2_t vrsqrte_f32_b(float32x2_t s){
#if defined(__APPLE__)
  return (float32x2_t)__builtin_neon_vrsqrte_v((int8x8_t)s, 9);
#elif defined(__aarch64__)
  return __builtin_aarch64_rsqrtev2sf(s);
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  return (float32x2_t)__builtin_neon_vrsqrtev2sf(s);
#elif defined(__x86_64__)
  float32x4_t r =_mm_rsqrt_ps((float32x4_t){s[0],s[1],0,0});
  return (float32x2_t){r[0],r[1]};
#else
# error "no vrsqrte_f32_b builtin"
#endif
}

typedef struct vector2 {
    float x,y;
} vector2;

static __inline__ __attribute__((always_inline)) float vmagnitude_vector2(float32x2_t v)
{
    float32x2_t sq = vmul_f32_b(v, v);
    float32x2_t s = vpadd_f32_b(sq, sq);
    s = vmax_f32_b(s, vdup_n_f32_b(1e-20f));
    float32x2_t rinv = vrsqrte_f32_b(s);
    return 1.f/rinv[0];
}

static inline float vector2_magnitude(vector2 v)
{
    float32x2_t xy = vld1_f32_b(&v.x);  // [x, y]
    return vmagnitude_vector2(xy);
}
static inline vector2 vector2_sub(vector2 a, vector2 b){
    return (vector2){a.x-b.x,a.y-b.y};
}

static inline vector2 vector2_add(vector2 a, vector2 b){
    return (vector2){a.x+b.x,a.y+b.y};
}

static inline vector2 vector2_norm(vector2 in){
    float len = vector2_magnitude(in);
    return (vector2){in.x/len,in.y/len};
}

static inline vector2 vector2_scale(vector2 in, float s){
    return (vector2){ in.x * s, in.y * s};
}

static inline bool vector2_zero(vector2 a){
    return float_zero(a.x) && float_zero(a.y);
}

static inline float dot_product(vector2 a, vector2 b) {
    return (a.x*b.x) + (a.y*b.y);
}

static inline vector2 vector2_lerp(vector2 a, vector2 b, float f){
    return (vector2){ lerpf(a.x, b.x, f), lerpf(a.y, b.y, f)};
}