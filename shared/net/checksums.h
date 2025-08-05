#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif
uint16_t checksum16(const uint16_t *data, size_t len);

uint16_t checksum16_pipv4(uint32_t src_ip,
                        uint32_t dst_ip,
                        uint8_t  protocol,
                        const uint8_t *payload,
                        uint16_t length);

#ifdef __cplusplus
}
#endif
