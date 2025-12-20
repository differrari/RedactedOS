#pragma once

#include "types.h"
#include "net/network_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
} icmpv6_hdr_t;

void icmpv6_input(uint16_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], uint8_t hop_limit, const uint8_t src_mac[6], const uint8_t *icmp, uint32_t icmp_len);
bool icmpv6_send_echo_request(const uint8_t dst_ip[16], uint16_t id, uint16_t seq, const void *payload, uint32_t payload_len, const void *tx_opts_or_null);
bool icmpv6_send_on_l2(uint8_t ifindex, const uint8_t dst_ip[16], const uint8_t src_ip[16], const uint8_t dst_mac[6], const void *icmp, uint32_t icmp_len, uint8_t hop_limit);

#ifdef __cplusplus
}
#endif
