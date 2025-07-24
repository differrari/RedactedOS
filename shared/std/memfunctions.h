#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

int memcmp(const void *s1, const void *s2, unsigned long count);
void* memset(void* dest, uint32_t val, size_t count);
void* memcpy(void *dest, const void *src, uint64_t count);

#ifdef __cplusplus
}
#endif