#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool dns_cache_get_ip(const char* name, uint8_t rr_type, uint8_t out_addr[16]);
void dns_cache_put_ip(const char* name, uint8_t rr_type,const uint8_t addr[16], uint32_t ttl_ms);
void dns_cache_tick(uint32_t ms);

#ifdef __cplusplus
}
#endif
