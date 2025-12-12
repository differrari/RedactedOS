#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    sizedptr key;
    sizedptr value;
} QueryParam;

void query_parse(const char *buf, uint32_t len, QueryParam **out_params, uint32_t *out_count);
void query_parse_z(const char *buf, QueryParam **out_params, uint32_t *out_count);

#ifdef __cplusplus
}
#endif
