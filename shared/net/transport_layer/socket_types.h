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

typedef enum {
    SOCK_OPT_DEBUG = 1u << 0,
    SOCK_OPT_KEEPALIVE = 1u << 1,
    SOCK_OPT_BUF_SIZE = 1u << 2,
    SOCK_OPT_DONTFRAG = 1u << 3,
    SOCK_OPT_TTL = 1u << 4,
    SOCK_OPT_MCAST_JOIN = 1u << 5
} SockOptFlags;

typedef enum {
    SOCK_DBG_LOW = 0,
    SOCK_DBG_MEDIUM = 1,
    SOCK_DBG_ALL = 2
} SockDebugLevel;

typedef struct SocketExtraOptions {
    uint32_t flags;
    SockDebugLevel debug_level;
    uint32_t buf_size;
    uint32_t keepalive_ms;
    uint8_t ttl;
    ip_version_t mcast_ver;
    uint8_t mcast_group[16];
} SocketExtraOptions;

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
