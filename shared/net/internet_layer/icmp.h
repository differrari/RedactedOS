#pragma once
#include "types.h"
#include "net/network_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO_REQUEST 8

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
    uint8_t payload[56];
} icmp_packet;

typedef struct {
    bool response; //1 replay 0 request
    uint16_t id;
    uint16_t seq;
    uint8_t payload[56];
} icmp_data;

void create_icmp_packet(uintptr_t p,
                        const net_l2l3_endpoint *src,
                        const net_l2l3_endpoint *dst,
                        const icmp_data *data);

void icmp_input(uintptr_t ptr,
                uint32_t len,
                uint32_t src_ip,
                uint32_t dst_ip);

void icmp_send_echo(uint32_t dst_ip,
                    uint16_t id,
                    uint16_t seq,
                    const uint8_t payload[56]);

bool icmp_ping(uint32_t dst_ip,
            uint16_t id,
            uint16_t seq,
            uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
