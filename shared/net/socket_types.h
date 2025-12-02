#pragma once
#include "types.h"
#include "net/network_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BIND_L3 = 0,
    BIND_L2 = 1,
    BIND_IP = 2,
    BIND_ANY = 3
} SockBindKind;

typedef enum {
    DST_ENDPOINT = 0,
    DST_DOMAIN = 1
} SockDstKind;

typedef struct SockBindSpec{
    SockBindKind kind;
    ip_version_t ver;
    uint8_t l3_id;
    uint8_t ifindex;
    uint8_t ip[16];
} SockBindSpec;

#ifdef __cplusplus
}
#endif
