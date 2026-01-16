#pragma once

#include "types.h"
#include "net/network_types.h"
#include "networking/internet_layer/icmp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ICMPV6_DEST_UNREACH 1
#define ICMPV6_PACKET_TOO_BIG 2
#define ICMPV6_TIME_EXCEEDED 3
#define ICMPV6_PARAM_PROBLEM 4
#define ICMPV6_ECHO_REQUEST 128
#define ICMPV6_ECHO_REPLY 129

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
} icmpv6_hdr_t;

typedef struct {
    uint32_t rtt_ms;
    uint8_t status;
    uint8_t icmp_type;
    uint8_t icmp_code;
    uint8_t _pad;
    uint8_t responder_ip[16];
} ping6_result_t;

void icmpv6_input(uint16_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], uint8_t hop_limit, const uint8_t src_mac[6], const uint8_t *icmp, uint32_t icmp_len);
bool icmpv6_ping(const uint8_t dst_ip[16], uint16_t id, uint16_t seq, uint32_t timeout_ms, const void *tx_opts_or_null, uint8_t hop_limit, ping6_result_t *out);
bool icmpv6_send_on_l2(uint8_t ifindex, const uint8_t dst_ip[16], const uint8_t src_ip[16], const uint8_t dst_mac[6], const void *icmp, uint32_t icmp_len, uint8_t hop_limit);

#ifdef __cplusplus
}
#endif
