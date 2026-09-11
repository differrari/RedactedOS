#pragma once
#include "types.h"
#include "net/network_types.h"

#ifdef __cplusplus
extern "C" {
#endif

uint16_t pmtu_get(l3_id_t l3_id, uint32_t l3_epoch, ip_version_t ver, const void* dst);
uint16_t pmtu_note(l3_id_t l3_id, uint32_t l3_epoch, ip_version_t ver, const void* dst, uint16_t mtu);

#ifdef __cplusplus
}
#endif
