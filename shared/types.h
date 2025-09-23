#pragma once

#ifndef asm
#define asm __asm__
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define bswap16(v) __builtin_bswap16((uint16_t)(v))
#define bswap32(v) __builtin_bswap32((uint32_t)(v))
#define bswap64(v) __builtin_bswap64((uint64_t)(v))

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  #define be16(v) bswap16(v)
  #define be32(v) bswap32(v)
  #define be64(v) bswap64(v)
#else
  #define be16(v) ((uint16_t)(v))
  #define be32(v) ((uint32_t)(v))
  #define be64(v) ((uint64_t)(v))
#endif

#define rd_be16(p) ( ((uint16_t)((const uint8_t*)(p))[0] << 8)  | \
                     ((uint16_t)((const uint8_t*)(p))[1]) )

#define rd_be32(p) ( ((uint32_t)((const uint8_t*)(p))[0] << 24) | \
                     ((uint32_t)((const uint8_t*)(p))[1] << 16) | \
                     ((uint32_t)((const uint8_t*)(p))[2] << 8)  | \
                     ((uint32_t)((const uint8_t*)(p))[3]) )

#define wr_be16(p, v) do { \
    (p)[0] = (uint8_t)((v) >> 8); \
    (p)[1] = (uint8_t)(v); \
} while(0)

#define wr_be32(p, v) do { \
    (p)[0] = (uint8_t)((v) >> 24); \
    (p)[1] = (uint8_t)((v) >> 16); \
    (p)[2] = (uint8_t)((v) >> 8);  \
    (p)[3] = (uint8_t)(v); \
} while(0)

typedef unsigned int uint32_t;
typedef long unsigned int size_t;
typedef unsigned long uint64_t;
typedef unsigned long uintptr_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

#define UINT64_MAX 0xFFFFFFFFFFFFFFFFULL
#define UINT16_MAX 0xFFFF
#define UINT32_MAX 0xFFFFFFFF

#define N_ARR(arr) (sizeof(arr)/sizeof((arr)[0]))

typedef signed int int32_t;
typedef signed long int64_t;
typedef signed long intptr_t;
typedef signed short int16_t;
typedef signed char int8_t;

#define INT16_MAX  0x7FFF

typedef struct sizedptr {
    uintptr_t ptr;
    size_t size;
} sizedptr;

#define NULL 0

#ifdef __cplusplus
}
#else

typedef unsigned char bool;

#define true 1
#define false 0

#endif