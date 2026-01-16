#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DNS_SD_MDNS_PORT 5353

#define DNS_SD_TYPE_A 1
#define DNS_SD_TYPE_PTR 12
#define DNS_SD_TYPE_TXT 16
#define DNS_SD_TYPE_SRV 33
#define DNS_SD_TYPE_AAAA 28
#define DNS_SD_TYPE_ANY 255

#define DNS_SD_CLASS_IN 1

#define DNS_SD_FLAG_QR 0x8000
#define DNS_SD_FLAG_AA 0x0400

#define DNS_SD_DOMAIN_LOCAL "local"
#define DNS_SD_ENUM_SERVICES "_services._dns-sd._udp.local"

uint32_t dns_sd_encode_qname(uint8_t *out, uint32_t cap, uint32_t off, const char *name);
uint32_t dns_sd_put_u16(uint8_t *out, uint32_t cap, uint32_t off, uint16_t v);
uint32_t dns_sd_put_u32(uint8_t *out, uint32_t cap, uint32_t off, uint32_t v);

uint32_t dns_sd_add_rr_ptr(uint8_t *out, uint32_t cap, uint32_t off, const char *name, uint16_t rrclass, uint32_t ttl_s, const char *target);
uint32_t dns_sd_add_rr_a(uint8_t *out, uint32_t cap, uint32_t off, const char *name, uint16_t rrclass, uint32_t ttl_s, uint32_t ip);
uint32_t dns_sd_add_rr_aaaa(uint8_t *out, uint32_t cap, uint32_t off, const char *name, uint16_t rrclass, uint32_t ttl_s, const uint8_t ip6[16]);
uint32_t dns_sd_add_rr_srv(uint8_t *out, uint32_t cap, uint32_t off, const char *name, uint16_t rrclass, uint32_t ttl_s, uint16_t priority, uint16_t weight, uint16_t port, const char *target);
uint32_t dns_sd_add_rr_txt(uint8_t *out, uint32_t cap, uint32_t off, const char *name, uint16_t rrclass, uint32_t ttl_s, const char *txt);

#ifdef __cplusplus
}
#endif
