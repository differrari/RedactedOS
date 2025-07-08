/*
 * PRNG API - Simple Pseudo-Random Number Generator
 * 
 * Usage:
 *   #include "rng/prng.h"
 * 
 *   // Seed the PRNG at startup
 *   prng_seed(1234);
 * 
 *   // Get a random integer in [min, max)
 *   uint32_t r = prng_rng(0, 100);
 * 
 *   // Fill a buffer with random bytes
 *   uint8_t buf[16];
 *   prng_bytes(buf, sizeof(buf));
 * 
 * Functions:
 *   void prng_seed(uint32_t seed);               // Seed the generator
 *   uint32_t prng_rng(uint32_t min, uint32_t max); // Random integer in [min, max)
 *   void prng_bytes(uint8_t *buf, size_t num_bytes); // Fill buffer with random bytes
 */

#ifndef PRNG_H
#define PRNG_H

#include <stdint.h>
#include <stddef.h>

// Set the seed for the PRNG
void prng_seed(uint32_t seed);

// Get a random number in [min, max)
uint32_t prng_rng(uint32_t min, uint32_t max);

// Fill buf with num_bytes random bytes
void prng_bytes(uint8_t *buf, size_t num_bytes);

#endif // PRNG_H
