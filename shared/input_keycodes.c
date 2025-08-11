#include "input_keycodes.h"

char hid_to_char(unsigned char c){
    return hid_keycode_to_char[c];
}