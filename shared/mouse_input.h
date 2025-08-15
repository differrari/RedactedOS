#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif 

typedef struct {
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t scroll;
} mouse_input;

#ifdef __cplusplus
}
#endif 