#pragma once

#include "types.h"
#include "graphic_types.h"

#if __cplusplus
extern "C" {
#endif

typedef enum {
    cursor_invalid,
    cursor_pointer,
    cursor_crosshair,
    cursor_pencil,
    // cursor_hand,
    // cursor_ibeam,
    // cursor_hresize,
    // cursor_wresize,
    // cursor_dresize,
} cursor_types;

extern cursor_types current_cursor_type;

void default_cursor();
bool switch_cursor(cursor_types);
void list_cursors();

#define CURSOR_WIDTH 64
#define CURSOR_HEIGHT 64

draw_ctx get_cursor();//TODO: tint
draw_ctx get_resized_cursor(int width, int height);

//TODO: maybe this file should OWN the cursor memory? and recycle it

#if __cplusplus
}
#endif