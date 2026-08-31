#pragma once
#include "types.h"
#include "net/network_types.h"
#include "networking/netpkt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ICMP_ECHO_REPLY = 0,
    ICMP_DEST_UNREACH = 3,
    ICMP_REDIRECT = 5,
    ICMP_ECHO_REQUEST = 8,
    ICMP_TIME_EXCEEDED = 11,
    ICMP_PARAM_PROBLEM = 12
} icmp_type_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_echo_hdr_t;

void icmp_input(uint8_t ifindex, netpkt_t* pkt, uint32_t src_ip, uint32_t dst_ip);

#ifdef __cplusplus
}
#endif
