#include "serial_input.h"
#include "input/input_dispatch.h"
#include "exceptions/timer.h"
#include "console/kio.h"

u8 last_serial_byte = 0;
u64 last_received = 0;

void emulate_key(u8 hid, bool press){
    register_event((kbd_event){
        .type = press ? KEY_PRESS : KEY_RELEASE,
        .key = press ? hid : 0
    });
    register_keypress((keypress){
        .keys[0] = press ? hid : 0,
    });
}

void process_serial_input(u8 byte){
    emulate_key(char_to_hid[byte], true);
    emulate_key(char_to_hid[byte], false);
}