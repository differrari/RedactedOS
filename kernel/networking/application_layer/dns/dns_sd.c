#include "dns_sd.h"
#include "std/std.h"

uint32_t dns_sd_encode_qname(uint8_t* out, uint32_t cap, uint32_t off, const char* name) {
    if(!out) return 0;
    if(!cap) return 0;
    if(off >= cap) return 0;
    if(!name) return 0;

    uint32_t idx = off;
    uint32_t lab_len = 0;
    uint32_t lab_pos = idx;

    out[idx] = 0;
    idx++;

    while (*name) {
        if(*name == '.') {
            if(lab_len > 63) return 0;
            out[lab_pos] = (uint8_t)lab_len;
            lab_len = 0;
            lab_pos = idx;
            if(idx >= cap) return 0;
            out[idx] = 0;
            idx++;
            name++;
            continue;
        }

        if(idx >= cap) return 0;
        out[idx] = (uint8_t)(*name);
        idx++;
        name++;
        lab_len++;
        if(lab_len > 63) return 0;
    }

    if(lab_len > 63) return 0;
    if(lab_pos >= cap) return 0;
    out[lab_pos] = (uint8_t)lab_len;
    if(idx >= cap) return 0;
    out[idx] = 0;
    idx++;
    return idx;
}

uint32_t dns_sd_put_u16(uint8_t* out, uint32_t cap, uint32_t off, uint16_t v) {
    if(!out) return 0;
    if(off + 2 > cap) return 0;
    uint16_t t = be16(v);
    memcpy(out + off, &t, 2);
    return off + 2;
}

uint32_t dns_sd_put_u32(uint8_t* out, uint32_t cap, uint32_t off, uint32_t v) {
    if(!out) return 0;
    if(off + 4 > cap) return 0;
    uint32_t t = be32(v);
    memcpy(out + off, &t, 4);
    return off + 4;
}

uint32_t dns_sd_add_rr_ptr(uint8_t* out, uint32_t cap, uint32_t off, const char* name, uint16_t rrclass, uint32_t ttl_s, const char* target) {
    off = dns_sd_encode_qname(out, cap, off, name);
    if(!off) return 0;

    off = dns_sd_put_u16(out, cap, off, DNS_SD_TYPE_PTR);
    if(!off) return 0;
    off = dns_sd_put_u16(out, cap, off, rrclass);
    if(!off) return 0;
    off = dns_sd_put_u32(out, cap, off, ttl_s);
    if(!off) return 0;

    uint32_t rdlen_pos = off;
    off = dns_sd_put_u16(out, cap,off, 0);
    if(!off) return 0;

    uint32_t r0 = off;
    off = dns_sd_encode_qname(out, cap, off, target);
    if(!off) return 0;

    uint16_t rdlen = (uint16_t)(off - r0);
    uint16_t rdbe = be16(rdlen);
    memcpy(out + rdlen_pos, &rdbe, 2);
    return off;
}

uint32_t dns_sd_add_rr_a(uint8_t* out, uint32_t cap, uint32_t off, const char* name, uint16_t rrclass, uint32_t ttl_s, uint32_t ip) {
    off = dns_sd_encode_qname(out, cap, off, name);
    if(!off) return 0;

    off = dns_sd_put_u16(out, cap,off,DNS_SD_TYPE_A);
    if(!off) return 0;
    off = dns_sd_put_u16(out,cap, off, rrclass);
    if(!off) return 0;
    off = dns_sd_put_u32(out, cap, off, ttl_s);
    if(!off) return 0;
    off = dns_sd_put_u16(out, cap, off, 4);
    if(!off) return 0;

    if(off + 4 > cap) return 0;
    out[off + 0] = (uint8_t)(ip >> 24);
    out[off + 1] = (uint8_t)(ip >> 16);
    out[off + 2] = (uint8_t)(ip >> 8);
    out[off + 3] = (uint8_t)(ip);
    return off + 4;
}

uint32_t dns_sd_add_rr_aaaa(uint8_t* out, uint32_t cap, uint32_t off, const char* name, uint16_t rrclass,uint32_t ttl_s, const uint8_t ip6[16]) {
    off = dns_sd_encode_qname(out, cap, off, name);
    if(!off) return 0;

    off = dns_sd_put_u16(out, cap, off, DNS_SD_TYPE_AAAA);
    if(!off) return 0;
    off = dns_sd_put_u16(out, cap, off, rrclass);
    if(!off) return 0;
    off = dns_sd_put_u32(out, cap, off, ttl_s);
    if(!off) return 0;
    off = dns_sd_put_u16(out, cap, off, 16);
    if(!off) return 0;

    if(off + 16 > cap) return 0;
    memcpy(out + off, ip6, 16);
    return off + 16;
}

uint32_t dns_sd_add_rr_srv(uint8_t* out, uint32_t cap, uint32_t off, const char* name, uint16_t rrclass, uint32_t ttl_s, uint16_t priority, uint16_t weight, uint16_t port, const char* target) {
    if(!out) return 0;
    if(!cap) return 0;
    if(off >= cap) return 0;
    if(!name) return 0;
    if(!target) return 0;

    off = dns_sd_encode_qname(out, cap, off, name);
    if(!off) return 0;

    off = dns_sd_put_u16(out, cap, off, DNS_SD_TYPE_SRV);
    if(!off) return 0;
    off = dns_sd_put_u16(out, cap, off, rrclass);
    if(!off) return 0;
    off = dns_sd_put_u32(out, cap, off, ttl_s);
    if(!off) return 0;

    uint32_t rdlen_pos = off;
    off = dns_sd_put_u16(out, cap, off, 0);
    if(!off) return 0;

    uint32_t rdata_start = off;

    off = dns_sd_put_u16(out, cap, off, priority);
    if(!off) return 0;
    off = dns_sd_put_u16(out, cap, off, weight);
    if(!off) return 0;
    off = dns_sd_put_u16(out, cap, off, port);
    if(!off) return 0;

    off = dns_sd_encode_qname(out, cap, off, target);
    if(!off) return 0;

    uint16_t rdlen = (uint16_t)(off - rdata_start);
    uint16_t t = be16(rdlen);
    memcpy(out + rdlen_pos, &t, 2);
    return off;
}

uint32_t dns_sd_add_rr_txt(uint8_t *out, uint32_t cap, uint32_t off, const char *name, uint16_t rrclass, uint32_t ttl_s, const char *txt) {
    if(!out) return 0;
    if(!cap) return 0;
    if(off >= cap) return 0;
    if(!name) return 0;

    off = dns_sd_encode_qname(out, cap, off, name);
    if(!off) return 0;

    off = dns_sd_put_u16(out, cap, off, DNS_SD_TYPE_TXT);
    if(!off) return 0;
    off = dns_sd_put_u16(out, cap, off, rrclass);
    if(!off) return 0;
    off = dns_sd_put_u32(out, cap, off, ttl_s);
    if(!off) return 0;

    uint32_t rdlen_pos = off;
    off = dns_sd_put_u16(out, cap, off, 0);
    if(!off) return 0;

    uint32_t rdata_start = off;

    if(txt && txt[0]){
        const char* p = txt;
        while(*p) {
            while(*p == ' ' || *p == '\t' || *p == ';' || *p == '\n' || *p == '\r') p++;
            if(!*p) break;

            const char* start = p;
            while(*p && *p != ';' && *p != '\n' && *p != '\r') p++;

            uint32_t len = (uint32_t)(p - start);
            if(len > 255) len = 255;

            if(off + 1 + len > cap) return 0;
            out[off] = (uint8_t)len;
            off++;
            memcpy(out + off, start,len);
            off += len;

            while(*p == ' ' || *p == '\t' || *p == ';' || *p == '\n' || *p == '\r') p++;
        }
    }

    uint16_t rdlen = (uint16_t)(off - rdata_start);
    uint16_t t = be16(rdlen);
    memcpy(out + rdlen_pos, &t, 2);
    return off;
}
