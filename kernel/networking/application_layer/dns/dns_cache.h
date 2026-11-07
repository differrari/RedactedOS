#pragma once
#include "types.h"
#include "net/network_types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool dns_cache_get_ip(l3_id_t l3_id, const char* name, uint16_t rr_type, uint8_t out_addr[16]);
void dns_cache_put_ip(l3_id_t l3_id, const char* name, uint16_t rr_type,const uint8_t addr[16], uint32_t ttl_ms);
void dns_cache_remove_ip(l3_id_t l3_id, const char* name, uint16_t rr_type);

#ifdef __cplusplus
}
#endif
