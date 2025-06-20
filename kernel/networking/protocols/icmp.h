#pragma once

#include <Security/cssmconfig.h>
#include <cstdint>
#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "net/network_types.h"

typedef struct __attribute__((packed)) icmp_packet {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_packet;

void create_icmp_packet(uintptr_t p);
bool icmp_should_handle(icmp_packet *icmp, uint32_t ip);
void icmp_populate_response(network_connection_ctx *ctx, icmp_packet* icmp);

#ifdef __cplusplus
}
#endif