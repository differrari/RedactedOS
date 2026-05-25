#include "dns_wire.h"
#include "std/std.h"
#include "std/string.h"

uint32_t dns_wire_put_u16(uint8_t *out, uint32_t cap, uint32_t off, uint16_t v) {
    if (!out) return 0;
    if (off + 2 > cap) return 0;
    wr_be16(out + off, v);
    return off + 2;
}

uint32_t dns_wire_put_u32(uint8_t *out, uint32_t cap, uint32_t off, uint32_t v) {
    if (!out) return 0;
    if (off + 4 > cap) return 0;
    wr_be32(out + off, v);
    return off + 4;
}

bool dns_wire_name_normalize(const char *name, char *out, uint32_t out_cap) {
    if (!name) return false;
    if (!out) return false;
    if (!out_cap) return false;

    uint32_t in_len = strlen(name);
    while (in_len && name[in_len - 1] == '.') in_len--;
    if (!in_len) return false;
    if (in_len >= out_cap) return false;

    uint32_t label_len = 0;
    for (uint32_t i = 0; i < in_len; i++) {
        char c = tolower(name[i]);
        if (c == '.') {
            if (!label_len) return false;
            label_len = 0;
            out[i] = '.';
            continue;
        }
        if (c <= 32) return false;
        label_len++;
        if (label_len > 63) return false;
        out[i] = c;
    }

    if (!label_len) return false;
    out[in_len] = 0;
    return true;
}

bool dns_wire_name_equals(const char *a, const char *b) {
    char na[DNS_WIRE_MAX_NAME];
    char nb[DNS_WIRE_MAX_NAME];
    if (!dns_wire_name_normalize(a, na, sizeof(na))) return false;
    if (!dns_wire_name_normalize(b, nb, sizeof(nb))) return false;
    return strcmp(na, nb) == 0;
}

bool dns_wire_is_local_name(const char *name) {
    char n[DNS_WIRE_MAX_NAME];
    if (!dns_wire_name_normalize(name, n, sizeof(n))) return false;
    return strend(n, ".local") == 0;
}

bool dns_wire_write_name(uint8_t *out, uint32_t cap, uint32_t *off, const char *name) {
    if (!out) return false;
    if (!off) return false;
    if (!name) return false;
    if (*off >= cap) return false;

    uint32_t in_len = strlen(name);
    while (in_len && name[in_len - 1] == '.') in_len--;
    if (!in_len) return false;
    if (in_len >= DNS_WIRE_MAX_NAME) return false;

    uint32_t idx = *off;
    uint32_t wire_len = 1;
    uint32_t pos = 0;
    while (pos < in_len) {
        uint32_t start = pos;
        uint32_t len = 0;
        while (pos < in_len && name[pos] != '.') {
            char c = name[pos];
            if (c <= 32) return false;
            len++;
            pos++;
        }
        if (!len || len > 63) return false;
        wire_len += 1 + len;
        if (wire_len > 255) return false;
        if (idx + 1 + len > cap) return false;

        out[idx++] = len;
        memcpy(out + idx, name + start, len);
        idx += len;

        if (pos < in_len) {
            if (name[pos] != '.') return false;
            pos++;
            if (pos == in_len) return false;
        }
    }

    if (idx >= cap) return false;
    out[idx++] = 0;
    *off = idx;
    return true;
}

bool dns_wire_read_name(const uint8_t *msg, uint32_t msg_len, uint32_t off, char *out, uint32_t out_cap, uint32_t *out_next) {
    if (!msg) return false;
    if (!out) return false;
    if (!out_cap) return false;
    if (off >= msg_len) return false;

    uint32_t idx = off;
    uint32_t out_idx = 0;
    uint32_t jumps = 0;
    bool jumped = false;

    while (true) {
        if (idx >= msg_len) return false;
        uint8_t c = msg[idx];

        if ((c & 0xC0) == 0xC0) {
            if (idx + 1 >= msg_len) return false;
            uint16_t ptr = ((c & 0x3F) << 8) | msg[idx+1];
            if (ptr >= msg_len) return false;
            if (!jumped) {
                if (out_next) *out_next = idx + 2;
                jumped = true;
            }
            idx = ptr;
            jumps++;
            if (jumps > 16) return false;
            continue;
        }

        if (c & 0xC0) return false;

        idx++;
        if (!c) {
            if (!jumped && out_next) *out_next = idx;
            if (!out_idx) {
                if (out_cap < 2) return false;
                out[0] = '.';
                out[1] = 0;
                return true;
            }
            if (out_idx >= out_cap) return false;
            out[out_idx] = 0;
            return true;
        }

        uint32_t label_len = c;
        if (label_len > 63) return false;
        if (idx + label_len > msg_len) return false;

        if (out_idx) {
            if (out_idx + 1 >= out_cap) return false;
            out[out_idx++] = '.';
        }

        if (out_idx + label_len >= out_cap) return false;
        memcpy(out + out_idx, msg + idx, label_len);
        out_idx += label_len;
        idx += label_len;
    }
}

bool dns_wire_read_rr(const uint8_t *msg, uint32_t msg_len, uint32_t off, dns_section_t section, dns_rr_view_t *rr, uint32_t *out_next) {
    if (!rr) return false;
    memset(rr, 0, sizeof(*rr));
    if (!dns_wire_read_name(msg, msg_len, off, rr->name, sizeof(rr->name), &off)) return false;
    if (off + 10 > msg_len) return false;

    rr->type = rd_be16(msg + off);
    rr->rrclass = rd_be16(msg + off + 2);
    rr->ttl_s = rd_be32(msg + off + 4);
    rr->rdlen = rd_be16(msg + off + 8);
    rr->rdata_off = off + 10;
    rr->section = section;
    if (rr->rdata_off + rr->rdlen > msg_len) return false;
    if (out_next) *out_next = rr->rdata_off + rr->rdlen;
    return true;
}

bool dns_wire_parse_rdata(const uint8_t *msg, uint32_t msg_len, const dns_rr_view_t *rr, dns_record_t *out) {
    if (!msg) return false;
    if (!rr) return false;
    if (!out) return false;
    if (rr->rdata_off + rr->rdlen > msg_len) return false;

    memset(out, 0, sizeof(*out));
    strncpy(out->name, rr->name, sizeof(out->name));
    out->type = rr->type;
    out->rrclass = rr->rrclass;
    out->ttl_s = rr->ttl_s;
    out->section = rr->section;

    const uint8_t *rdata = msg + rr->rdata_off;
    if (rr->type == DNS_TYPE_A) {
        if (rr->rdlen != 4)return false;
        memcpy(out->addr, rdata, 4);
        return true;
    }

    if (rr->type == DNS_TYPE_AAAA) {
        if (rr->rdlen != 16)return false;
        memcpy(out->addr, rdata, 16);
        return true;
    }

    if (rr->type == DNS_TYPE_CNAME || rr->type == DNS_TYPE_PTR || rr->type == DNS_TYPE_NS) {
        uint32_t next = 0;
        if (!rr->rdlen)return false;
        if (!dns_wire_read_name(msg, msg_len, rr->rdata_off, out->target, sizeof(out->target), &next)) return false;
        return next == rr->rdata_off + rr->rdlen;
    }

    if (rr->type == DNS_TYPE_TXT) {
        uint32_t idx = 0;
        uint32_t out_idx = 0;
        out->txt[0] = 0;
        while (idx < rr->rdlen) {
            uint8_t len = rdata[idx++];
            if (idx + len > rr->rdlen) return false;
            if (len) {
                if (out_idx) {
                    if (out_idx+1 >= sizeof(out->txt)) return true;
                    out->txt[out_idx++] = ';';
                }
                uint32_t copy = len;
                if (out_idx + copy >= sizeof(out->txt)) copy = sizeof(out->txt) - out_idx - 1;
                memcpy(out->txt + out_idx, rdata + idx, copy);
                out_idx += copy;
            }
            idx += len;
        }
        out->txt[out_idx] = 0;
        return true;
    }

    if (rr->type == DNS_TYPE_SRV) {
        if (rr->rdlen < 7) return false;
        out->priority = rd_be16(rdata);
        out->weight = rd_be16(rdata + 2);
        out->port = rd_be16(rdata + 4);
        uint32_t next = 0;
        if (!dns_wire_read_name(msg, msg_len, rr->rdata_off + 6, out->target, sizeof(out->target), &next)) return false;
        return next == rr->rdata_off + rr->rdlen;
    }

    return true;
}

bool dns_wire_parse_records(const uint8_t *msg, uint32_t msg_len, bool check_id, uint16_t message_id, dns_record_t *out, uint32_t out_cap, uint32_t *out_count, uint16_t *out_flags) {
    if (out_count) *out_count = 0;
    if (!msg) return false;
    if (msg_len < 12) return false;
    if (check_id && rd_be16(msg) != message_id) return false;

    uint16_t flags = rd_be16(msg+2);
    uint16_t qd = rd_be16(msg+4);
    uint16_t an = rd_be16(msg+6);
    uint16_t ns = rd_be16(msg + 8);
    uint16_t ar = rd_be16(msg + 10);
    if (out_flags) *out_flags = flags;

    uint32_t off = 12;
    for (uint16_t i = 0; i < qd; i++) {
        if (!dns_wire_read_name(msg, msg_len, off, NULL, 0, &off)) return false;
        if (off + 4 > msg_len) return false;
        off += 4;
    }

    uint32_t count = 0;
    uint32_t section_count[3];
    dns_section_t section[3];
    section_count[0] = an;
    section_count[1] = ns;
    section_count[2] = ar;
    section[0] = DNS_SECTION_ANSWER;
    section[1] = DNS_SECTION_AUTHORITY;
    section[2] = DNS_SECTION_ADDITIONAL;

    for (uint32_t s = 0; s < 3; s++) {
        for (uint32_t i = 0; i < section_count[s]; i++) {
            dns_rr_view_t rr;
            if (!dns_wire_read_rr(msg, msg_len, off, section[s], &rr, &off)) return false;
            if (out && count < out_cap && dns_wire_parse_rdata(msg, msg_len, &rr, &out[count])) count++;
        }
    }

    if (out_count) *out_count = count;
    return true;
}

uint32_t dns_wire_build_query(uint8_t *out, uint32_t cap, uint16_t message_id, const char *name,uint16_t qtype, bool mdns_qu) {
    if (!out) return 0;
    if (cap < 12) return 0;

    memset(out, 0, cap);
    wr_be16(out, message_id);
    wr_be16(out + 2, message_id ? DNS_FLAG_RD : 0);
    wr_be16(out + 4,1);

    uint32_t off = 12;
    if (!dns_wire_write_name(out, cap, &off, name)) return 0;
    if (off + 4 > cap) return 0;
    wr_be16(out + off, qtype);
    wr_be16(out + off + 2, mdns_qu?  DNS_CLASS_CACHE_FLUSH | DNS_CLASS_IN : DNS_CLASS_IN);
    return off + 4;
}
