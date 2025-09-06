#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif 

typedef struct {
    uint8_t modifier;
    uint8_t rsvd;
    char keys[6];
} keypress;

//TODO: make a bool key_pressed(KEYCODE, keypress*)

#ifdef __cplusplus
}
#endif 