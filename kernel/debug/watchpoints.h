#pragma once

#include "types.h"

typedef enum {
    watchpoint_type_write = 0b10,
    watchpoint_type_read  = 0b01,
    watchpoint_type_all = watchpoint_type_write | watchpoint_type_read
} debug_watchpoint_type;

int debug_watch(uptr address, debug_watchpoint_type type, u8 bit_mask);
bool debug_unwatch(int index);