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

typedef enum in_event_type { KEY_RELEASE, KEY_PRESS, MOD_RELEASE, MOD_PRESS } in_event_type;

typedef struct {
    in_event_type type;
    char key;
    char modifier;
} kbd_event;

#ifdef __cplusplus
}
#endif 