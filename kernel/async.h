#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif
void delay(uint32_t count);
bool wait(uint32_t *reg, uint32_t expected_value, bool match, uint32_t timeout);
#ifdef __cplusplus
}
#endif