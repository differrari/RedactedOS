#include "ipv4_utils.h"

static char* u8_to_str(uint8_t val, char* out) {
    if (val >= 100) {
        *out++ = '0' + (val / 100);
        val %= 100;
        *out++ = '0' + (val / 10);
        *out++ = '0' + (val % 10);
    } else if (val >= 10) {
        *out++ = '0' + (val / 10);
        *out++ = '0' + (val % 10);
    } else {
        *out++ = '0' + val;
    }
    return out;
}

bool ipv4_is_unspecified(uint32_t ip) { return ip == 0; }
bool ipv4_is_loopback(uint32_t ip) { return (ip & 0xFF000000u) == 0x7F000000u; }
bool ipv4_is_multicast(uint32_t ip) { return (ip & 0xF0000000u) == 0xE0000000u; }
bool ipv4_is_link_local(uint32_t ip) { return (ip & 0xFFFF0000u) == 0xA9FE0000u; }
bool ipv4_is_private(uint32_t ip) {
    if ((ip & 0xFF000000u) == 0x0A000000u) return true;
    if ((ip & 0xFFF00000u) == 0xAC100000u) return true;
    if ((ip & 0xFFFF0000u) == 0xC0A80000u) return true;
    return false;
}
bool ipv4_is_cgnat(uint32_t ip) { return (ip & 0xFFC00000u) == 0x64400000u; }
bool ipv4_is_documentation(uint32_t ip) {
    if ((ip & 0xFFFFFF00u) == 0xC0000200u) return true;
    if ((ip & 0xFFFFFF00u) == 0xC6336400u) return true;
    if ((ip & 0xFFFFFF00u) == 0xCB007100u) return true;
    return false;
}
bool ipv4_is_benchmark(uint32_t ip) { return (ip & 0xFFFE0000u) == 0xC6120000u; }
bool ipv4_is_reserved(uint32_t ip) {
    if ((ip & 0xF0000000u) == 0xF0000000u) return true;
    if ((ip & 0xFF000000u) == 0xFF000000u) return true;
    return false;
}
bool ipv4_is_reserved_special(uint32_t ip) {
    if (ipv4_is_unspecified(ip)) return true;
    if (ipv4_is_loopback(ip)) return true;
    if (ipv4_is_link_local(ip)) return true;
    if (ipv4_is_multicast(ip)) return true;
    if (ipv4_is_reserved(ip)) return true;
    return false;
}
bool ipv4_is_unicast_global(uint32_t ip) {
    if (ipv4_is_unspecified(ip)) return false;
    if (ipv4_is_loopback(ip)) return false;
    if (ipv4_is_multicast(ip)) return false;
    if (ipv4_is_link_local(ip)) return false;
    if (ipv4_is_private(ip)) return false;
    if (ipv4_is_cgnat(ip)) return false;
    if (ipv4_is_documentation(ip)) return false;
    if (ipv4_is_benchmark(ip)) return false;
    if (ipv4_is_reserved(ip)) return false;
    return true;
}

bool ipv4_mask_is_contiguous(uint32_t mask) {
    if (mask == 0) return true;
    return ((mask | (mask - 1u)) == 0xFFFFFFFFu);
}

int ipv4_prefix_len(uint32_t mask) {
    int n = 0;
    while (mask & 0x80000000u) { n++; mask <<= 1; }
    return n;
}

uint32_t ipv4_net(uint32_t ip, uint32_t mask) { return ip & mask; }
uint32_t ipv4_broadcast_calc(uint32_t ip, uint32_t mask) { return (mask == 0) ? 0 : ((ip & mask) | ~mask); }

bool ipv4_is_network_address(uint32_t ip, uint32_t mask) {
    if (!ipv4_mask_is_contiguous(mask)) return false;
    if (mask == 0 || mask == 0xFFFFFFFFu) return false;
    return (ip & ~mask) == 0;
}

bool ipv4_is_broadcast_address(uint32_t ip, uint32_t mask) {
    if (!ipv4_mask_is_contiguous(mask)) return false;
    if (mask == 0 || mask == 0xFFFFFFFFu) return false;
    return (ip & ~mask) == ~mask;
}

bool ipv4_is_limited_broadcast(uint32_t ip) { return ip == 0xFFFFFFFFu; }

bool ipv4_is_directed_broadcast(uint32_t ip, uint32_t mask, uint32_t dst) {
    if (!ipv4_mask_is_contiguous(mask)) return false;
    if (mask == 0 || mask == 0xFFFFFFFFu) return false;
    return ipv4_broadcast_calc(ip, mask) == dst;
}

bool ipv4_same_subnet(uint32_t a, uint32_t b, uint32_t mask) {
    if (!ipv4_mask_is_contiguous(mask)) return false;
    return (a & mask) == (b & mask);
}

void ipv4_to_string(uint32_t ip, char* buf) {
    uint8_t a = (uint8_t)(ip >> 24);
    uint8_t b = (uint8_t)(ip >> 16);
    uint8_t c = (uint8_t)(ip >> 8);
    uint8_t d = (uint8_t)(ip);
    char* p = buf;
    p = u8_to_str(a, p); *p++ = '.';
    p = u8_to_str(b, p); *p++ = '.';
    p = u8_to_str(c, p); *p++ = '.';
    p = u8_to_str(d, p);
    *p = '\0';
}

bool ipv4_parse(const char* s, uint32_t* out) {
    if (!s || !out) return false;
    uint32_t ip = 0, v = 0;
    int oct = 0, digits = 0;
    const char* p = s;
    while (*p) {
        if (*p == '.') {
            if (digits == 0 || v > 255 || oct >= 3) return false;
            ip = (ip << 8) | (v & 0xFF);
            v = 0;
            digits = 0;
            oct++;
        } else if (*p >= '0' && *p <= '9') {
            v = v * 10 + (uint32_t)(*p - '0');
            if (v > 255) return false;
            digits++;
        } else {
            return false;
        }
        ++p;
    }
    if (oct != 3 || digits == 0 || v > 255) return false;
    ip = (ip << 8) | (v & 0xFF);
    *out = ip;
    return true;
}

void ipv4_mcast_to_mac(uint32_t group, uint8_t out_mac[6]) {
    if (!out_mac) return;
    out_mac[0] = 0x01;
    out_mac[1] = 0x00;
    out_mac[2] = 0x5e;
    out_mac[3] = (uint8_t)((group >> 16) & 0x7Fu);
    out_mac[4] = (uint8_t)((group >> 8) & 0xFFu);
    out_mac[5] = (uint8_t)(group & 0xFFu);
}
