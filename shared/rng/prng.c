/*
 * PRNG Implementation - Simple Linear Congruential Generator (LCG)
 *
 * How to use:
 *   #include "rng/prng.h"
 *   prng_seed(1234);       // Seed the generator
 *   uint32_t r = prng_rng(0, 100); // Random integer in [0, 100)
 *   uint8_t buf[8];
 *   prng_bytes(buf, 8);    // Fill buf with 8 random bytes
 *
 * See prng.h for API details.
 */

#include "prng.h"

static uint32_t prng_state = 1;

void prng_seed(uint32_t seed) {
    prng_state = seed ? seed : 1; // avoid zero seed
}

uint32_t prng_rng(uint32_t min, uint32_t max) {
    // Linear congruential generator, Numerical Recipes
    prng_state = prng_state * 1664525u + 1013904223u;
    if (max <= min) return min;
    return min + (prng_state % (max - min));
}

void prng_bytes(uint8_t *buf, size_t num_bytes) {
    for (size_t i = 0; i < num_bytes; ++i) {
        // New random number every 4 bytes
        if (i % 4 == 0) {
            prng_state = prng_state * 1664525u + 1013904223u;
        }
        buf[i] = (prng_state >> ((i % 4) * 8)) & 0xFF;
    }
}
