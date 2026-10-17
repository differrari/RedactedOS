#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NetDevKind : uint8_t {
    NET_DEV_ETH = 0x00,
    NET_DEV_WIFI = 0x01,
    NET_DEV_OTHER = 0x02,
    NET_DEV_LOOPBACK = 0xFE,
    NET_DEV_UNKNOWN = 0xFF
} NetDevKind;

typedef enum NetLinkKind : uint8_t {
    NET_LINK_DIRECT = 0,
    NET_LINK_VLAN = 1,
    NET_LINK_LOOPBACK = 2
} NetLinkKind;

typedef enum LinkDuplex : uint8_t {
    LINK_DUPLEX_HALF = 0,
    LINK_DUPLEX_FULL = 1,
    LINK_DUPLEX_UNKNOWN = 0xFF
} LinkDuplex;

#ifdef __cplusplus
}
#endif
