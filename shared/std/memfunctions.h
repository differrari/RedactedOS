#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

int memcmp(const void *s1, const void *s2, unsigned long n);
void *memset(void *dest, int val, unsigned long count);
void *memcpy(void *dest, const void *src, uint64_t n);

#ifdef __cplusplus
}
#endif