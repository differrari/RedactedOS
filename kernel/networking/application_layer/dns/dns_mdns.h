#pragma once
#include "dns.h"
#include "types.h"

dns_result_t mdns_query(const char* name, dns_qtype_t qtype, uint32_t timeout_ms, dns_record_t* out_records, uint32_t max_records, uint32_t* out_count);
dns_result_t mdns_resolve_a(const char* name, uint32_t timeout_ms, uint32_t* out_ip, uint32_t* out_ttl_s);
dns_result_t mdns_resolve_aaaa(const char* name, uint32_t timeout_ms, uint8_t out_ipv6[16], uint32_t* out_ttl_s);