#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void ipv4_to_string(uint32_t ip, char* buf);
bool ipv4_parse(const char* s, uint32_t* out);

#ifdef __cplusplus
}
#endif