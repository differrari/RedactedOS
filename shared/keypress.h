#pragma once

#include "types.h"

typedef struct {
    uint8_t modifier;
    uint8_t rsvd;
    char keys[6];
} keypress;