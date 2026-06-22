#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAC_ADDR_LEN 6

void mac_copy(uint8_t dst[MAC_ADDR_LEN], const uint8_t src[MAC_ADDR_LEN]);
bool mac_equal(const uint8_t a[MAC_ADDR_LEN], const uint8_t b[MAC_ADDR_LEN]);
void mac_clear(uint8_t mac[MAC_ADDR_LEN]);
void mac_set_broadcast(uint8_t mac[MAC_ADDR_LEN]);
void mac_to_string(const uint8_t mac[MAC_ADDR_LEN], char out[18]);

#ifdef __cplusplus
}
#endif
