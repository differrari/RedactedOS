#pragma once

#include "types.h"
#include "net/network_types.h"
#include "net/socket_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ICMP_PROBE_OK = 0,
    ICMP_PROBE_TIMEOUT = 1,
    ICMP_PROBE_NET_UNREACH = 2,
    ICMP_PROBE_HOST_UNREACH = 3,
    ICMP_PROBE_PROTO_UNREACH = 4,
    ICMP_PROBE_PORT_UNREACH = 5,
    ICMP_PROBE_FRAG_NEEDED = 6,
    ICMP_PROBE_SRC_ROUTE_FAILED = 7,
    ICMP_PROBE_ADMIN_PROHIBITED = 8,
    ICMP_PROBE_TTL_EXPIRED = 9,
    ICMP_PROBE_PARAM_PROBLEM = 10,
    ICMP_PROBE_REDIRECT = 11,
    ICMP_PROBE_UNKNOWN_ERROR = 255
} icmp_probe_status_t;

typedef struct {
    uint32_t rtt_ms;
    net_l4_endpoint responder;
    uint8_t status;
    uint8_t icmp_type;
    uint8_t icmp_code;
    uint8_t reserved;
} icmp_probe_result_t;

bool icmp_probe_parse_bind(const char* arg, SockBindSpec* out);
uint32_t icmp_probe_collect(const net_l4_endpoint* dst, uint16_t id, uint16_t seq, uint32_t timeout_ms, const SockBindSpec* bind, uint8_t ttl, icmp_probe_result_t* out, uint32_t max_results);

#ifdef __cplusplus
}
#endif
