#include "ipv6_utils.h"
#include "std/std.h"
#include "networking/network.h"

bool ipv6_is_unspecified(const uint8_t ip[16]) {
    for (int i = 0; i < 16; i++) if (ip[i] != 0) return false;
    return true;
}

bool ipv6_is_loopback(const uint8_t ip[16]) {
    for (int i = 0; i < 15; i++) if (ip[i] != 0) return false;
    return ip[15] == 1;
}

bool ipv6_is_multicast(const uint8_t ip[16]) { return ip[0] == 0xFF; }

bool ipv6_is_ula(const uint8_t ip[16]) { return (ip[0] & 0xFE) == 0xFC; }

bool ipv6_is_linklocal(const uint8_t ip[16]) { return ip[0] == 0xFE && (ip[1] & 0xC0) == 0x80; }

int ipv6_cmp(const uint8_t a[16], const uint8_t b[16]) {
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    return 0;
}

void ipv6_cpy(uint8_t dst[16], const uint8_t src[16]) { memcpy(dst, src, 16); }

int ipv6_common_prefix_len(const uint8_t a[16], const uint8_t b[16]) {
    int bits = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t x = (uint8_t)(a[i] ^ b[i]);
        if (x == 0) { bits += 8; continue; }
        for (int bpos = 7; bpos >= 0; --bpos) {
            if (x & (1u << bpos)) return bits + (7 - bpos);
        }
    }
    return 128;
}

void ipv6_make_multicast(uint8_t scope, ipv6_mcast_kind_t kind, const uint8_t unicast[16], uint8_t out[16]) {
    memset(out, 0, 16);
    out[0] = 0xFF;
    out[1] = scope & 0x0F;

    switch (kind) {
        case IPV6_MCAST_ALL_NODES:
            out[15] = 0x01;
            break;
        case IPV6_MCAST_MDNS:
            out[15] = 0xFB;
            break;
        case IPV6_MCAST_ALL_ROUTERS:
            out[15] = 0x02;
            break;
        case IPV6_MCAST_SSDP:
            out[15] = 0x0c;
            break;
        case IPV6_MCAST_DHCPV6_SERVERS:
            out[11] = 0x00;
            out[12] = 0x01;
            out[13] = 0x00;
            out[14] = 0x00;
            out[15] = 0x02;
            break;
        case IPV6_MCAST_MLDV2_ROUTERS:
            out[11] = 0x00;
            out[12] = 0x00;
            out[13] = 0x00;
            out[14] = 0x00;
            out[15] = 0x16;
            break;
        case IPV6_MCAST_SOLICITED_NODE:
        default:
            out[11] = 0x01;
            out[12] = 0xFF;
            out[13] = unicast ? unicast[13] : 0;
            out[14] = unicast ? unicast[14] : 0;
            out[15] = unicast ? unicast[15] : 0;
            break;
    }
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ipv6_parse(const char* s, uint8_t out[16]) {
    if (!s || !out) return false;

    uint16_t words[8] = { 0 };
    int wi = 0, zpos = -1;
    const char* p = s;

    if (p[0] == ':' && p[1] == ':') {
        zpos = 0;
        p += 2;
    }

    while (*p) {
        if (wi >= 8) return false;

        int val = 0, cnt = 0, hv;
        while ((hv = hexval(*p)) >= 0) {
            val = (val << 4) | hv;
            cnt++;
            if (cnt > 4) return false;
            p++;
        }
        if (cnt == 0) return false;

        words[wi++] = (uint16_t)val;

        if (*p == 0) break;
        if (*p != ':') return false;

        if (*(p + 1) == ':') {
            if (zpos >= 0) return false;
            zpos = wi;
            p += 2;
            if (*p == 0) break;
        } else {
            p++;
        }
    }

    int fill = 8 - wi, o = 0;
    if (zpos < 0 && wi != 8) return false;

    for (int i = 0; i < 8; i++) {
        uint16_t w;
        if (zpos >= 0) {
            if (i < zpos) w = words[i];
            else if (i < zpos + fill) w = 0;
            else w = words[i - fill];
        } else {
            w = words[i];
        }
        out[o++] = (uint8_t)(w >> 8);
        out[o++] = (uint8_t)(w & 0xFF);
    }

    return true;
}

void ipv6_to_string(const uint8_t ip[16], char* buf, int buflen) {
    uint16_t w[8];
    for (int i = 0; i < 8; i++) w[i] = (uint16_t)((ip[2 * i] << 8) | ip[2 * i + 1]);

    int best_s = -1, best_l = 0, cur_s = -1, cur_l = 0;
    for (int i = 0; i < 8; i++) {
        if (w[i] == 0) {
            if (cur_s < 0) { cur_s = i; cur_l = 1; } else cur_l++;
            if (cur_l > best_l) {
                best_l = cur_l;
                best_s = cur_s;
            }
        } else {
            cur_s = -1;
            cur_l = 0;
        }
    }
    if (best_l <2) {
        best_s = -1;
        best_l = 0;
    }

    int n = 0;
    int need_colon = 0;

    for (int i = 0; i < 8; ) {
        if (best_l > 0 && i == best_s) {
            if (n < buflen) buf[n++] = ':';
            if (n < buflen) buf[n++] = ':';
            need_colon = 0;
            i += best_l;
            if (i >= 8) break;
            continue;
        }

        if (need_colon) {
            if (n < buflen) buf[n++] = ':';
        }

        int v = w[i];
        int started = 0;
        for (int sh = 12; sh >= 0; sh -= 4) {
            int d = (v >> sh) & 0xF;
            if (!started && d == 0 && sh > 0) continue;
            started = 1;
            if (n < buflen) buf[n++] = "0123456789abcdef"[d];
        }
        if (!started) {
            if (n < buflen) buf[n++] = '0';
        }

        need_colon = 1;
        i++;
    }

    if (n < buflen) buf[n] = 0;
    else if (buflen > 0) buf[buflen - 1] = 0;
}

void ipv6_multicast_mac(const uint8_t ip[16], uint8_t mac[6]) {
    mac[0] = 0x33;
    mac[1] = 0x33;
    mac[2] = ip[12];
    mac[3] = ip[13];
    mac[4] = ip[14];
    mac[5] = ip[15];
}

void ipv6_make_lla_from_mac(uint8_t ifindex, uint8_t out[16]) {
    const uint8_t* mac = network_get_mac(ifindex);
    memset(out, 0, 16);
    out[0] = 0xFE;
    out[1] = 0x80;
    if (!mac) {
        out[8] = 0x02 ^ 0x02;
        out[9] = ifindex;
        out[10] = 0x00;
        out[11] = 0xFF;
        out[12] = 0xFE;
        out[13] = ifindex;
        out[14] = 0x00;
        out[15] = 0x01;
        return;
    }
    out[8] = mac[0]^ 0x02;
    out[9] = mac[1];
    out[10] = mac[2];
    out[11] = 0xFF;
    out[12] = 0xFE;
    out[13] = mac[3];
    out[14] = mac[4];
    out[15] = mac[5];
}