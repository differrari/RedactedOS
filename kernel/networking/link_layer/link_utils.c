#include "link_utils.h"
#include "std/memory.h"

void mac_copy(uint8_t dst[MAC_ADDR_LEN], const uint8_t src[MAC_ADDR_LEN]){
    if (!dst || !src) return;
    memcpy(dst, src, MAC_ADDR_LEN);
}

bool mac_equal(const uint8_t a[MAC_ADDR_LEN], const uint8_t b[MAC_ADDR_LEN]){
    if (!a || !b) return false;
    return memcmp(a, b, MAC_ADDR_LEN) == 0;
}

void mac_clear(uint8_t mac[MAC_ADDR_LEN]){
    if (!mac) return;
    memset(mac, 0, MAC_ADDR_LEN);
}

void mac_set_broadcast(uint8_t mac[MAC_ADDR_LEN]){
    if (!mac) return;
    memset(mac, 0xFF, MAC_ADDR_LEN);
}

void mac_to_string(const uint8_t mac[MAC_ADDR_LEN], char out[18]){
    if (!out) return;
    static const char HEX[] = "0123456789abcdef";
    int p = 0;
    for (int i = 0; i < MAC_ADDR_LEN; ++i) {
        uint8_t b = mac ? mac[i] : 0;
        out[p++] = HEX[b >> 4];
        out[p++] = HEX[b & 0x0F];
        if (i != MAC_ADDR_LEN - 1) out[p++] = ':';
    }
    out[p] = 0;
}
