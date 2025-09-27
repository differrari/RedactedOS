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

typedef enum in_event_type { KEY_RELEASE, KEY_PRESS } in_event_type;

typedef struct {
    in_event_type type;
    char key;
} kbd_event;

#ifdef __cplusplus
}
#endif 