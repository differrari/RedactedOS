#include "link_utils.h"

void mac_to_string(const uint8_t mac[6], char out[18]){
    static const char HEX[] = "0123456789abcdef";
    int p = 0;
    for (int i = 0; i < 6; ++i) {
        uint8_t b = mac ? mac[i] : 0;
        out[p++] = HEX[b >> 4];
        out[p++] = HEX[b & 0x0F];
        if (i != 5) out[p++] = ':';
    }
    out[p] = 0;
}
