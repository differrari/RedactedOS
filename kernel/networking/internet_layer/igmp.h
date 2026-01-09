#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool igmp_send_join(uint8_t ifindex, uint32_t group);
bool igmp_send_leave(uint8_t ifindex, uint32_t group);
void igmp_input(uint8_t ifindex, uint32_t src, uint32_t dst, const void* l4, uint32_t l4_len);

#ifdef __cplusplus
}
#endif
