#pragma once

#include "types.h"
#include "ui/graphic_types.h"

#ifdef __cplusplus
extern "C" {
#endif 

typedef struct {
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t scroll;
} mouse_input;

typedef struct {
    mouse_input raw;
    gpu_point position;
} mouse_data;

static inline bool mouse_button_down(mouse_data* m, uint8_t button){
    return (m->raw.buttons >> button) & 1;
}

#ifdef __cplusplus
}
#endif 