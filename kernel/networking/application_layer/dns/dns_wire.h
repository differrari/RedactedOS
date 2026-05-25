#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DNS_WIRE_MAX_NAME 256
#define DNS_WIRE_MAX_TXT 256

#define DNS_CLASS_IN 1
#define DNS_CLASS_ANY 255
#define DNS_CLASS_CACHE_FLUSH 0x8000
#define DNS_CLASS_MASK 0x7FFF

#define DNS_FLAG_QR 0x8000
#define DNS_FLAG_AA 0x0400
#define DNS_FLAG_RD 0x0100

#define DNS_RCODE_MASK 0x000F
#define DNS_RCODE_NXDOMAIN 3

#define DNS_MDNS_PORT 5353
#define DNS_MDNS_GROUP_V4 0xE00000FB

#define DNS_TYPE_A 1
#define DNS_TYPE_NS 2
#define DNS_TYPE_CNAME 5
#define DNS_TYPE_SOA 6
#define DNS_TYPE_PTR 12
#define DNS_TYPE_MX 15
#define DNS_TYPE_TXT 16
#define DNS_TYPE_AAAA 28
#define DNS_TYPE_SRV 33
#define DNS_TYPE_OPT 41
#define DNS_TYPE_DS 43
#define DNS_TYPE_NAPTR 35
#define DNS_TYPE_CAA 257
#define DNS_TYPE_ANY 255

//TODO add type handling for SOA OPT MX NS NAPTR DS CAA when needed

typedef enum {
    DNS_SECTION_ANSWER = 0,
    DNS_SECTION_AUTHORITY = 1,
    DNS_SECTION_ADDITIONAL = 2
} dns_section_t;

typedef struct {
    char name[DNS_WIRE_MAX_NAME];
    uint16_t type;
    uint16_t rrclass;
    uint32_t ttl_s;
    uint16_t rdlen;
    uint32_t rdata_off;
    dns_section_t section;
} dns_rr_view_t;

typedef struct {
    char name[DNS_WIRE_MAX_NAME];
    uint16_t type;
    uint16_t rrclass;
    uint32_t ttl_s;
    dns_section_t section;
    uint8_t addr[16];
    char target[DNS_WIRE_MAX_NAME];
    char txt[DNS_WIRE_MAX_TXT];
    uint16_t priority;
    uint16_t weight;
    uint16_t port;
} dns_record_t;

uint32_t dns_wire_put_u16(uint8_t *out, uint32_t cap, uint32_t off, uint16_t v);
uint32_t dns_wire_put_u32(uint8_t *out, uint32_t cap, uint32_t off, uint32_t v);
bool dns_wire_write_name(uint8_t *out, uint32_t cap, uint32_t *off, const char *name);
bool dns_wire_read_name(const uint8_t *msg, uint32_t msg_len, uint32_t off, char *out, uint32_t out_cap, uint32_t *out_next);
bool dns_wire_name_normalize(const char *name, char *out, uint32_t out_cap);
bool dns_wire_name_equals(const char *a, const char *b);
bool dns_wire_is_local_name(const char *name);
bool dns_wire_read_rr(const uint8_t *msg, uint32_t msg_len, uint32_t off, dns_section_t section, dns_rr_view_t *rr, uint32_t *out_next);
bool dns_wire_parse_rdata(const uint8_t *msg, uint32_t msg_len, const dns_rr_view_t *rr, dns_record_t *out);
bool dns_wire_parse_records(const uint8_t *msg, uint32_t msg_len, bool check_id, uint16_t message_id, dns_record_t *out, uint32_t out_cap, uint32_t *out_count, uint16_t *out_flags);
uint32_t dns_wire_build_query(uint8_t *out, uint32_t cap, uint16_t message_id, const char *name,uint16_t qtype, bool mdns_qu);

#ifdef __cplusplus
}
#endif
