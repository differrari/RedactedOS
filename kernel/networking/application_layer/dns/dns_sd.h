#pragma once
#include "types.h"
#include "dns_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DNS_SD_DOMAIN_LOCAL "local"
#define DNS_SD_ENUM_SERVICES "_services._dns-sd._udp.local"

uint32_t dns_sd_add_rr_ptr(uint8_t *out, uint32_t cap, uint32_t off, const char *name, uint16_t rrclass, uint32_t ttl_s, const char *target);
uint32_t dns_sd_add_rr_a(uint8_t *out, uint32_t cap, uint32_t off, const char *name, uint16_t rrclass, uint32_t ttl_s, uint32_t ip);
uint32_t dns_sd_add_rr_aaaa(uint8_t *out, uint32_t cap, uint32_t off, const char *name, uint16_t rrclass, uint32_t ttl_s, const uint8_t ip6[16]);
uint32_t dns_sd_add_rr_srv(uint8_t *out, uint32_t cap, uint32_t off, const char *name, uint16_t rrclass, uint32_t ttl_s, uint16_t priority, uint16_t weight, uint16_t port, const char *target);
uint32_t dns_sd_add_rr_txt(uint8_t *out, uint32_t cap, uint32_t off, const char *name, uint16_t rrclass, uint32_t ttl_s, const char *txt);

#ifdef __cplusplus
}
#endif
