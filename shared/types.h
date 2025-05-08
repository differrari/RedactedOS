#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int uint32_t;
typedef unsigned int size_t;
typedef unsigned long uint64_t;
typedef unsigned long uintptr_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

typedef int int32_t;
typedef long int64_t;
typedef long intptr_t;
typedef short int16_t;
typedef char int8_t;

#define NULL 0

#ifdef __cplusplus
}
#else

typedef unsigned int bool;

#define true 1
#define false 0

#endif