#include "dns_sd.h"
#include "std/std.h"

uint32_t dns_sd_add_rr_ptr(uint8_t* out, uint32_t cap, uint32_t off, const char* name, uint16_t rrclass, uint32_t ttl_s, const char *target) {
    if (!dns_wire_write_name(out, cap, &off, name)) return 0;
    off = dns_wire_put_u16(out, cap, off, DNS_TYPE_PTR);
    if(!off) return 0;
    off = dns_wire_put_u16(out, cap, off, rrclass);
    if(!off) return 0;
    off = dns_wire_put_u32(out, cap, off, ttl_s);
    if(!off) return 0;

    uint32_t rdlen_pos = off;
    off = dns_wire_put_u16(out, cap,off, 0);
    if(!off) return 0;

    uint32_t r0 = off;
    if(!dns_wire_write_name(out, cap, &off, target)) return 0;

    uint16_t rdlen = (uint16_t)(off - r0);
    wr_be16(out + rdlen_pos, rdlen);
    return off;
}

uint32_t dns_sd_add_rr_a(uint8_t* out, uint32_t cap, uint32_t off, const char* name, uint16_t rrclass, uint32_t ttl_s, uint32_t ip) {
    if (!dns_wire_write_name(out, cap, &off, name)) return 0;

    off = dns_wire_put_u16(out, cap,off,DNS_TYPE_A);
    if(!off) return 0;
    off = dns_wire_put_u16(out,cap, off, rrclass);
    if(!off) return 0;
    off = dns_wire_put_u32(out, cap, off, ttl_s);
    if(!off) return 0;
    off = dns_wire_put_u16(out, cap, off, 4);
    if(!off) return 0;

    if(off + 4 > cap) return 0;
    wr_be32(out + off, ip);
    return off + 4;
}

uint32_t dns_sd_add_rr_aaaa(uint8_t* out, uint32_t cap, uint32_t off, const char* name, uint16_t rrclass,uint32_t ttl_s, const uint8_t ip6[16]) {
    if (!dns_wire_write_name(out, cap, &off, name)) return 0;

    off = dns_wire_put_u16(out, cap, off, DNS_TYPE_AAAA);
    if(!off) return 0;
    off = dns_wire_put_u16(out, cap, off, rrclass);
    if(!off) return 0;
    off = dns_wire_put_u32(out, cap, off, ttl_s);
    if(!off) return 0;
    off = dns_wire_put_u16(out, cap, off, 16);
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

    if (!dns_wire_write_name(out, cap, &off, name)) return 0;

    off = dns_wire_put_u16(out, cap, off, DNS_TYPE_SRV);
    if(!off) return 0;
    off = dns_wire_put_u16(out, cap, off, rrclass);
    if(!off) return 0;
    off = dns_wire_put_u32(out, cap, off, ttl_s);
    if(!off) return 0;

    uint32_t rdlen_pos = off;
    off = dns_wire_put_u16(out, cap, off, 0);
    if(!off) return 0;

    uint32_t rdata_start = off;

    off = dns_wire_put_u16(out, cap, off, priority);
    if(!off) return 0;
    off = dns_wire_put_u16(out, cap, off, weight);
    if(!off) return 0;
    off = dns_wire_put_u16(out, cap, off, port);
    if(!off) return 0;

    if (!dns_wire_write_name(out, cap, &off, target)) return 0;

    uint16_t rdlen = off - rdata_start;
    wr_be16(out + rdlen_pos, rdlen);
    return off;
}

uint32_t dns_sd_add_rr_txt(uint8_t *out, uint32_t cap, uint32_t off, const char *name, uint16_t rrclass, uint32_t ttl_s, const char *txt) {
    if(!out) return 0;
    if(!cap) return 0;
    if(off >= cap) return 0;
    if(!name) return 0;

    if (!dns_wire_write_name(out, cap, &off, name)) return 0;

    off = dns_wire_put_u16(out, cap, off, DNS_TYPE_TXT);
    if(!off) return 0;
    off = dns_wire_put_u16(out, cap, off, rrclass);
    if(!off) return 0;
    off = dns_wire_put_u32(out, cap, off, ttl_s);
    if(!off) return 0;

    uint32_t rdlen_pos = off;
    off = dns_wire_put_u16(out, cap, off, 0);
    if(!off) return 0;

    uint32_t rdata_start = off;

    if(txt && txt[0]){
        const char* p = txt;
        while(*p) {
            while(is_whitespace(*p) || *p == ';') p++;
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

            while(is_whitespace(*p) || *p == ';') p++;
        }
    }

    uint16_t rdlen = (uint16_t)(off - rdata_start);
    wr_be16(out + rdlen_pos, rdlen);
    return off;
}

uint32_t dns_sd_add_record(uint8_t *out, uint32_t cap, uint32_t off, const dns_record_t *record, uint16_t rrclass, uint32_t ttl_s) {
    if (!record) return 0;
    switch (record->type) {
        case DNS_TYPE_A:
            return dns_sd_add_rr_a(out, cap, off, record->name, rrclass, ttl_s, rd_be32(record->addr));
        case DNS_TYPE_AAAA:
            return dns_sd_add_rr_aaaa(out, cap, off, record->name, rrclass, ttl_s, record->addr);
        case DNS_TYPE_PTR:
            return dns_sd_add_rr_ptr(out, cap, off, record->name, rrclass, ttl_s, record->target);
        case DNS_TYPE_SRV:
            return dns_sd_add_rr_srv(out, cap, off, record->name, rrclass, ttl_s, record->priority, record->weight, record->port, record->target);
        case DNS_TYPE_TXT:
            return dns_sd_add_rr_txt(out, cap, off, record->name, rrclass, ttl_s, record->txt);
        default:
            return 0;
    }
}
