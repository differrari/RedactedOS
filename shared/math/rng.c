#include "rng.h"
#include "std/memfunctions.h"

void rng_init_random(rng_t *rng){
    uint64_t seed;
    asm volatile("mrs %0, cntvct_el0" : "=r"(seed));
    rng_seed(rng, seed);
}

void rng_seed(rng_t* rng, uint64_t seed){ 
    uint64_t z = seed + 0x9E3779B97F4A7C15;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
    z = (z ^ (z >> 27)) *0x94D049BB133111EB;
    rng->s0 = z ^ (z >> 31);

    z = seed + 0x9E3779B97F4A7C15 + 1;
    z = (z ^ (z >> 30)) *0xBF58476D1CE4E5B9;
    z = (z ^ (z >> 27))*0x94D049BB133111EB;
    rng->s1 = z ^ (z >> 31);
}

uint64_t rng_next64(rng_t* rng){
    uint64_t s0 = rng->s0;
    uint64_t s1 = rng->s1;
    uint64_t result = s0 + s1;

    s1 ^= s0;
    rng->s0 = rotl(s0, 55)^s1^(s1 << 14);
    rng->s1 = rotl(s1, 36);

    return result;
}

uint32_t rng_next32(rng_t* rng){
    return (uint32_t)(rng_next64(rng) >> 32);
}

uint16_t rng_next16(rng_t* rng){
    return (uint16_t)(rng_next64(rng) >> 48);
}

uint8_t rng_next8(rng_t* rng){
    return (uint8_t)(rng_next64(rng) >> 56);
}

uint64_t rng_between64(rng_t* rng, uint64_t min, uint64_t max){
    if (max <= min) return min;
    return rng_next64(rng) % (max - min) + min;
}

uint32_t rng_between32(rng_t* rng, uint32_t min, uint32_t max){
    if (max <= min) return min;
    return rng_next64(rng) % (max - min) + min;
}

uint16_t rng_between16(rng_t* rng, uint16_t min, uint16_t max){
    if (max <= min) return min;
    return (uint16_t)(rng_next64(rng) % (max - min)) + min;
}

uint8_t rng_between8(rng_t* rng, uint8_t min, uint8_t max){
    if (max <= min) return min;
    return (uint8_t)(rng_next64(rng) % (max - min)) + min;
}

void rng_fill64(rng_t* rng, uint64_t* dst, size_t count){
    for (uint32_t i = 0; i < count; i++)
        dst[i] = rng_next64(rng);
}

void rng_fill32(rng_t* rng, uint32_t* dst, size_t count){
    for (uint32_t i = 0; i < count; i++)
        dst[i] = rng_next32(rng);
}

void rng_fill16(rng_t* rng, uint16_t* dst, size_t count){
    for (uint32_t i = 0; i < count; i++)
        dst[i] = rng_next16(rng);
}

void rng_fill8(rng_t* rng, uint8_t* dst, size_t count){
    for (uint32_t i = 0; i < count; i++)
        dst[i] = rng_next8(rng);
}

void rng_fill_buf(rng_t* rng, void* dest, size_t count) {
    uint64_t *d64 = (uint64_t *)dest;

    uint64_t blocks = count / 32;
    rng_fill64(rng, dest, blocks);

    uint64_t remaining = (count % 32) / 8;
    for (uint64_t i = 0; i < remaining; i++) d64[i] = rng_next64(rng);;

    uint8_t *d8 = (uint8_t *)(d64 + remaining);
    for (uint64_t i = 0; i < count % 8; i++) d8[i] = rng_next8(rng);
}