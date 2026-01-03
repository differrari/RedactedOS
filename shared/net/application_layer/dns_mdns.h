#pragma once
#include "dns.h"
#include "types.h"

uint32_t mdns_encode_qname(uint8_t* dst, const char* name);
uint32_t mdns_skip_name(const uint8_t* message, uint32_t message_len, uint32_t offset);
bool mdns_read_name(const uint8_t* message, uint32_t message_len, uint32_t offset, char* out, uint32_t out_cap, uint32_t* consumed);

dns_result_t mdns_resolve_a(const char* name, uint32_t timeout_ms, uint32_t* out_ip, uint32_t* out_ttl_s);
dns_result_t mdns_resolve_aaaa(const char* name, uint32_t timeout_ms, uint8_t out_ipv6[16], uint32_t* out_ttl_s);