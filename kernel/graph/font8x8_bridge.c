#include "font8x8_basic.h"
#include "font8x8_bridge.h"

const uint8_t* get_font8x8(uint8_t c) {
    return font8x8_basic[c];
}