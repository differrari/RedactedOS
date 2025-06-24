#pragma once

#ifdef __cplusplus
extern "C" {
#endif
    
#include "types.h"

typedef enum NetProtocol {
    UDP,
    DHCP,
    ARP,
    TCP,
    ICMP
} NetProtocol;

uint16_t checksum16(uint16_t *data, size_t len);

uint16_t checksum16_pipv4(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol, const uint8_t* payload, uint16_t length);

typedef struct network_connection_ctx {
    uint16_t port;
    uint32_t ip;
    uint8_t mac[6];
} network_connection_ctx;

#ifdef __cplusplus
}
#endif