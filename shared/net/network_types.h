#pragma once

#ifdef __cplusplus
extern "C" {
#endif
    
#include "types.h"

typedef struct net_l4_endpoint {
    uint32_t ip;
    uint16_t port;
} net_l4_endpoint;
#ifdef __cplusplus
}
#endif