#pragma once

#ifdef __cplusplus
extern "C" {
#endif
    
#include "types.h"

typedef enum {
    IP_VER4 = 4,
    IP_VER6 = 6
} ip_version_t;

typedef struct net_l4_endpoint {
    ip_version_t ver;
    uint8_t ip[16];
    uint16_t port;
} net_l4_endpoint;

typedef enum {
    PROTO_UDP = 0,
    PROTO_TCP = 1
} protocol_t;

#ifdef __cplusplus
}
#endif