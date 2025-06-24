#pragma once

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
    uint8_t payload[56];
} icmp_packet;

typedef struct icmp_data {
    uint8_t response;
    uint16_t seq;
    uint16_t id;
    uint8_t payload[56];
} icmp_data;

void create_icmp_packet(uintptr_t p, network_connection_ctx source, network_connection_ctx destination, icmp_data *data);
uint16_t icmp_get_sequence(icmp_packet *packet);
uint16_t icmp_get_id(icmp_packet *packet);
void icmp_copy_payload(void* dest, icmp_packet *packet);

#ifdef __cplusplus
}
#endif