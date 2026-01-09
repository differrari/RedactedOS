#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void mac_to_string(const uint8_t mac[6], char out[18]);

#ifdef __cplusplus
}
#endif
