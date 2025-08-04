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

typedef struct net_l2l3_endpoint {
    uint8_t  mac[6];
    uint32_t ip; //rn ipv4 only
} net_l2l3_endpoint;

typedef struct net_l4_endpoint {
    uint32_t ip;
    uint16_t port;
} net_l4_endpoint;
#ifdef __cplusplus
}
#endif