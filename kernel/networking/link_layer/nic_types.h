#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NetIfKind : uint8_t {
    NET_IFK_ETH = 0x00,
    NET_IFK_WIFI = 0x01,
    NET_IFK_OTHER = 0x02,
    NET_IFK_LOCALHOST = 0xFE,
    NET_IFK_UNKNOWN = 0xFF
} NetIfKind;

typedef enum LinkDuplex : uint8_t {
    LINK_DUPLEX_HALF = 0,
    LINK_DUPLEX_FULL = 1,
    LINK_DUPLEX_UNKNOWN = 0xFF
} LinkDuplex;

#ifdef __cplusplus
}
#endif
