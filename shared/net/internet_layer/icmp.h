#pragma once
#include "types.h"
#include "net/network_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ICMP_ECHO_REPLY 0
#define ICMP_DEST_UNREACH 3
#define ICMP_REDIRECT 5
#define ICMP_ECHO_REQUEST 8
#define ICMP_TIME_EXCEEDED 11
#define ICMP_PARAM_PROBLEM 12

typedef enum {
    PING_OK = 0,
    PING_TIMEOUT = 1,
    PING_NET_UNREACH = 2,
    PING_HOST_UNREACH = 3,
    PING_PROTO_UNREACH = 4,
    PING_PORT_UNREACH = 5,
    PING_FRAG_NEEDED = 6,
    PING_SRC_ROUTE_FAILED = 7,
    PING_ADMIN_PROHIBITED = 8,
    PING_TTL_EXPIRED = 9,
    PING_PARAM_PROBLEM = 10,
    PING_REDIRECT = 11,
    PING_UNKNOWN_ERROR = 255
} ping_status_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
    uint8_t payload[56];
} icmp_packet;

typedef struct {
    uint32_t rtt_ms;
    uint8_t status;
    uint8_t icmp_type;
    uint8_t icmp_code;
    uint8_t _pad;
    uint32_t responder_ip;
} ping_result_t;

bool icmp_ping(uint32_t dst_ip, uint16_t id, uint16_t seq, uint32_t timeout_ms, const void *tx_opts_or_null, uint32_t ttl, ping_result_t *out);
void icmp_input(uintptr_t ptr, uint32_t len, uint32_t src_ip, uint32_t dst_ip);

#ifdef __cplusplus
}
#endif
