#include "dns_mdns.h"
#include "dns_daemon.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "std/std.h"
#include "networking/transport_layer/trans_utils.h"

#define MDNS_PORT 5353

static uint32_t skip_dns_name(const uint8_t* message, uint32_t message_len, uint32_t offset) {
    if (offset >= message_len) return message_len + 1;
    uint32_t cursor = offset;
    while (cursor < message_len) {
        uint8_t len = message[cursor];
        cursor++;
        if (len == 0) break;
        if ((len & 0xC0) == 0xC0) {
            if (cursor >= message_len) return message_len + 1;
            cursor++;
            break;
        }
        cursor +=len;
        if (cursor > message_len) return message_len + 1;
    }
    return cursor;
}

static bool read_dns_name(const uint8_t* message, uint32_t message_len, uint32_t offset, char* out, uint32_t out_cap, uint32_t* consumed) {
    if (!message) return false;
    if (!out) return false;
    if (!out_cap) return false;
    if (offset >= message_len) return false;

    uint32_t cur = offset;
    uint32_t out_len = 0;
    uint32_t consumed_local = 0;
    uint8_t jumped = 0;
    uint32_t jumps = 0;

    for (;;){
        if (cur >= message_len) return false;

        uint8_t len = message[cur];
        if (len == 0) {
            if (!jumped) consumed_local = cur - offset + 1;
            if (out_len >= out_cap) return false;
            out[out_len] = 0;
            if (consumed) *consumed = consumed_local;
            return true;
        }

        if ((len & 0xC0) == 0xC0) {
            if (cur + 1 >= message_len) return false;
            uint16_t ptr = (uint16_t)(((uint16_t)(len & 0x3F) << 8) | (uint16_t)message[cur + 1]);
            if (ptr >= message_len) return false;
            if (!jumped) consumed_local = cur - offset + 2;
            cur = ptr;
            jumped = 1;
            jumps++;
            if (jumps> 16) return false;
            continue;
        }

        cur++;
        if (cur + len > message_len) return false;

        if (out_len) {
            if (out_len + 1 >= out_cap) return false;
            out[out_len++] = '.';
        }

        if (out_len + len >= out_cap) return false;
        memcpy(out + out_len, message + cur, len);
        out_len += len;
        cur += len;

        if (!jumped) consumed_local = cur - offset;
    }
}

static dns_result_t parse_mdns_ip_record(const uint8_t* buffer, uint32_t buffer_len, const char* name, uint16_t qtype, uint8_t* out_rdata, uint32_t out_len, uint32_t* out_ttl_s) {
    if (!buffer) return DNS_ERR_FORMAT;
    if (buffer_len < 12) return DNS_ERR_FORMAT;
    if (!name) return DNS_ERR_FORMAT;
    if (!out_rdata) return DNS_ERR_FORMAT;
    if (!out_len) return DNS_ERR_FORMAT;

    uint16_t qd = rd_be16(buffer + 4);
    uint16_t an = rd_be16(buffer + 6);
    uint16_t ns = rd_be16(buffer + 8);
    uint16_t ar = rd_be16(buffer + 10);

    uint32_t offset = 12;
    for (uint16_t i = 0; i < qd; ++i) {
        offset = skip_dns_name(buffer, buffer_len, offset);
        if (offset+4 > buffer_len) return DNS_ERR_FORMAT;
        offset += 4;
    }

    uint32_t total = (uint32_t)an + (uint32_t)ns + (uint32_t)ar;
    uint32_t name_len = (uint32_t)strlen(name);

    for (uint32_t i = 0; i < total; ++i) {
        char rrname[256];
        uint32_t consumed = 0;
        if (!read_dns_name(buffer, buffer_len, offset, rrname, sizeof(rrname), &consumed)) return DNS_ERR_FORMAT;
        offset += consumed;

        if (offset + 10 > buffer_len) return DNS_ERR_FORMAT;

        uint16_t type = rd_be16(buffer + offset + 0);
        uint16_t klass = rd_be16(buffer + offset + 2);
        uint32_t ttl_s = rd_be32(buffer + offset + 4);
        uint16_t rdlen = rd_be16(buffer + offset + 8);
        offset += 10;

        if (offset + rdlen > buffer_len) return DNS_ERR_FORMAT;

        if (type == qtype && (klass & 0x7FFFu) == 1u){
            uint32_t rrname_len = (uint32_t)strlen(rrname);
            if (rrname_len == name_len&& strncmp(rrname, name, (int)name_len) == 0 && rdlen == out_len) {
                memcpy(out_rdata, buffer + offset, out_len);
                if (out_ttl_s) *out_ttl_s = ttl_s;
                return DNS_OK;
            }
        }

        offset += rdlen;
    }

    return DNS_ERR_NO_ANSWER;
}

static bool dns_write_qname(uint8_t* buf, uint32_t buf_len, uint32_t*inout_off, const char* name) {
    if (!buf || !inout_off || !name) return false;
    uint32_t off = *inout_off;
    if (off >= buf_len) return false;
    uint32_t label_len = 0;
    uint32_t label_pos = off;
    buf[off++] = 0;
    for (const char* p = name; *p; ++p) {
        char c = *p;
        if (c =='.') {
            if (!label_len || label_len > 63u) return false;
            buf[label_pos] = (uint8_t)label_len;
            label_len = 0;
            label_pos = off;
            if (off >= buf_len) return false;
            buf[off++] = 0;
            continue;
        }
        if (label_len >= 63u) return false;
        if (off >= buf_len) return false;
        buf[off++]= (uint8_t)c;
        label_len++;
    }
    if (!label_len || label_len > 63u) return false;
    buf[label_pos] = (uint8_t)label_len;
    if (off >= buf_len) return false;
    buf[off++] = 0;
    *inout_off = off;
    return true;
}

static dns_result_t perform_mdns_query_once(socket_handle_t sock, const net_l4_endpoint* dst, const char* name, uint16_t qtype, uint32_t timeout_ms, uint8_t* out_rdata, uint32_t out_len, uint32_t* out_ttl_s) {
    if (!sock) return DNS_ERR_NO_DNS;
    if (!dst) return DNS_ERR_NO_DNS;
    if (!name) return DNS_ERR_FORMAT;
    if (!out_rdata) return DNS_ERR_FORMAT;
    if (!out_len) return DNS_ERR_FORMAT;

    uint8_t request_buffer[512];
    memset(request_buffer, 0, sizeof(request_buffer));

    wr_be16(request_buffer + 0, 0);
    wr_be16(request_buffer + 2, 0x0000);
    wr_be16(request_buffer + 4, 1);

    uint32_t offset = 12;
	if (!dns_write_qname(request_buffer, (uint32_t)sizeof(request_buffer), &offset, name)) return DNS_ERR_FORMAT;
	if (offset + 4 > (uint32_t)sizeof(request_buffer)) return DNS_ERR_FORMAT;
    wr_be16(request_buffer + offset + 0, qtype);
    wr_be16(request_buffer + offset + 2, 0x0001);
    offset += 4;

    int64_t sent = socket_sendto_udp_ex(sock, DST_ENDPOINT, dst, 0, request_buffer, offset);
    if (sent < 0) return DNS_ERR_SEND;

    uint32_t waited_ms = 0;
    while (waited_ms < timeout_ms) {
        uint8_t response_buffer[512];
        net_l4_endpoint source;
        int64_t received = socket_recvfrom_udp_ex(sock, response_buffer, sizeof(response_buffer), &source);
        if (received > 0 && source.port == MDNS_PORT){
            uint32_t ttl_s = 0;
            dns_result_t pr = parse_mdns_ip_record(response_buffer, (uint32_t)received, name, qtype, out_rdata, out_len, &ttl_s);
            if (pr == DNS_OK){
                if (out_ttl_s) *out_ttl_s = ttl_s;
                return DNS_OK;
            }
        }

        msleep(20);
        waited_ms += 20;
    }

    return DNS_ERR_TIMEOUT;
}

dns_result_t mdns_resolve_a(const char* name, uint32_t timeout_ms, uint32_t* out_ip, uint32_t* out_ttl_s) {
    socket_handle_t sock = mdns_socket_handle_v4();
    if (!sock) return DNS_ERR_NO_DNS;

    uint32_t group = 0xE00000FBu;
    net_l4_endpoint dst;
    make_ep(group, MDNS_PORT, IP_VER4, &dst);

    uint8_t rdata[4];
    uint32_t ttl_s = 0;

    dns_result_t r = perform_mdns_query_once(sock, &dst, name, 1, timeout_ms, rdata, 4, &ttl_s);
    if (r != DNS_OK) return r;

    uint32_t ip;
    memcpy(&ip, rdata, 4);
    if (out_ip) *out_ip = rd_be32((uint8_t*)&ip);
    if (out_ttl_s) *out_ttl_s = ttl_s;
    return DNS_OK;
}

dns_result_t mdns_resolve_aaaa(const char* name, uint32_t timeout_ms, uint8_t out_ipv6[16], uint32_t* out_ttl_s) {
    socket_handle_t sock = mdns_socket_handle_v6();
    if (!sock) return DNS_ERR_NO_DNS;

    net_l4_endpoint dst;
    memset(&dst, 0, sizeof(dst));
    dst.ver = IP_VER6;
    ipv6_make_multicast(0x02, IPV6_MCAST_MDNS, 0, dst.ip);
    dst.port = MDNS_PORT;

    uint32_t ttl_s = 0;
    dns_result_t r = perform_mdns_query_once(sock, &dst, name, 28, timeout_ms, out_ipv6, 16, &ttl_s);
    if (r != DNS_OK) return r;

    if (out_ttl_s) *out_ttl_s = ttl_s;
    return DNS_OK;
}
