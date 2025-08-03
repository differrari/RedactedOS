#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

//xoroshiro128+, pseudorandom
typedef struct {
    uint64_t s0;
    uint64_t s1;
} rng_t;

static inline uint64_t rotl(uint64_t x, int k){
    return (x << k)|(x >> (64 - k));
}

void rng_init_random(rng_t* rng);
void rng_seed(rng_t* rng, uint64_t seed);

uint8_t  rng_next8(rng_t* rng);
uint16_t rng_next16(rng_t* rng);
uint32_t rng_next32(rng_t* rng);
uint64_t rng_next64(rng_t* rng);

uint8_t  rng_between8(rng_t* rng, uint8_t min, uint8_t max);
uint16_t rng_between16(rng_t* rng, uint16_t min, uint16_t max);
uint32_t rng_between32(rng_t* rng, uint32_t min, uint32_t max);
uint64_t rng_between64(rng_t* rng, uint64_t min, uint64_t max);

void rng_fill8(rng_t* rng, uint8_t* dst, size_t count);
void rng_fill16(rng_t* rng, uint16_t* dst, size_t count);
void rng_fill32(rng_t* rng, uint32_t* dst, size_t count);
void rng_fill64(rng_t* rng, uint64_t* dst, size_t count);
void rng_fill_buf(rng_t* rng, void* dst, size_t count);

#ifdef __cplusplus
}
#endif

