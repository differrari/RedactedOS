#include "dns.h"
#include "std/memory.h"
#include "math/rng.h"
#include "process/scheduler.h"
#include "net/internet_layer/ipv4.h"
#include "dns_daemon.h"
#include "types.h"

extern void sleep(uint64_t ms);

static uint32_t encode_dns_qname(uint8_t* dst, const char* name){
    uint32_t index = 0;
    uint32_t label_len = 0;
    uint32_t label_pos = 0;
    dst[index++] = 0;
    while (*name) {
        if (*name == '.') { dst[label_pos] = (uint8_t)label_len; label_len = 0; label_pos = index; dst[index++] = 0; name++; continue; }
        dst[index++] = (uint8_t)(*name); label_len++; name++;
    }
    dst[label_pos] = (uint8_t)label_len;
    dst[index++] = 0;
    return index;
}

static uint32_t skip_dns_name(const uint8_t* message, uint32_t message_len, uint32_t offset){
    if (offset >= message_len) return message_len + 1;
    uint32_t cursor = offset;
    while (cursor < message_len) {
        uint8_t len = message[cursor++];
        if (len == 0) break;
        if ((len & 0xC0) == 0xC0) { if (cursor >= message_len) return message_len + 1; cursor++; break; }
        cursor += len;
        if (cursor > message_len) return message_len + 1;
    }
    return cursor;
}

static dns_result_t parse_dns_a_record(uint8_t* buffer, uint32_t buffer_len, uint16_t message_id, uint32_t* out_ip){
    if (buffer_len < 12) return DNS_ERR_FORMAT;
    if (rd_be16(buffer+0) != message_id) return DNS_ERR_FORMAT;
    uint16_t flags = rd_be16(buffer+2);
    uint16_t question_count = rd_be16(buffer+4);
    uint16_t answer_count = rd_be16(buffer+6);
    if ((flags & 0x000F) == 3) return DNS_ERR_NXDOMAIN;
    uint32_t offset = 12;
    for (uint16_t i = 0; i < question_count; ++i){
        offset = skip_dns_name(buffer, buffer_len, offset);
        if (offset + 4 > buffer_len) return DNS_ERR_FORMAT;
        offset += 4;
    }
    for (uint16_t i = 0; i < answer_count; ++i){
        offset = skip_dns_name(buffer, buffer_len, offset);
        if (offset + 10 > buffer_len) return DNS_ERR_FORMAT;
        uint16_t type = rd_be16(buffer+offset+0);
        uint16_t klass = rd_be16(buffer+offset+2);
        uint16_t rdlength = rd_be16(buffer+offset+8);
        offset += 10;
        if (offset + rdlength > buffer_len) return DNS_ERR_FORMAT;
        if (type == 1 && klass == 1 && rdlength == 4){
            uint32_t ip_host = rd_be32(buffer+offset);
            *out_ip = ip_host;
            return DNS_OK;
        }
        offset += rdlength;
    }
    return DNS_ERR_NO_ANSWER;
}

static dns_result_t perform_dns_query_once(socket_handle_t sock, uint32_t dns_ip_host, const char* name, uint32_t timeout_ms, uint32_t* out_ip){
    uint8_t request_buffer[512]; memset(request_buffer,0,sizeof(request_buffer));
    rng_t rng; rng_init_random(&rng);
    uint16_t message_id = (uint16_t)(rng_next32(&rng) & 0xFFFF);
    wr_be16(request_buffer+0, message_id);
    wr_be16(request_buffer+2, 0x0100);
    wr_be16(request_buffer+4, 1);
    uint32_t offset = 12;
    offset += encode_dns_qname(request_buffer+offset, name);
    wr_be16(request_buffer+offset+0, 1);
    wr_be16(request_buffer+offset+2, 1);
    offset += 4;
    int64_t sent = socket_sendto_udp(sock, dns_ip_host, 53, request_buffer, offset);
    if (sent < 0) return DNS_ERR_SEND;
    uint32_t waited_ms = 0;
    while (waited_ms < timeout_ms){
        uint8_t response_buffer[512];
        uint32_t source_ip; uint16_t source_port;
        int64_t received = socket_recvfrom_udp(sock, response_buffer, sizeof(response_buffer), &source_ip, &source_port);
        if (received > 0 && source_port == 53 && source_ip == dns_ip_host){
            uint32_t ip_host;
            dns_result_t pr = parse_dns_a_record(response_buffer, (uint32_t)received, message_id, &ip_host);
            if (pr == DNS_OK){ *out_ip = ip_host; return DNS_OK; }
            if (pr == DNS_ERR_NXDOMAIN) return pr;
        } else {
            sleep(50);
            waited_ms += 50;
        }
    }
    return DNS_ERR_TIMEOUT;
}

dns_result_t dns_resolve_a(const char* hostname, uint32_t* out_ip, dns_server_sel_t which, uint32_t timeout_ms){
    const net_cfg_t* cfg = ipv4_get_cfg();
    if (!cfg || !cfg->rt) return DNS_ERR_NO_DNS;
    uint32_t dns_primary = cfg->rt->dns[0];
    uint32_t dns_secondary = cfg->rt->dns[1];
    if (which == DNS_USE_PRIMARY && dns_primary == 0) return DNS_ERR_NO_DNS;
    if (which == DNS_USE_SECONDARY && dns_secondary == 0) return DNS_ERR_NO_DNS;
    if (which == DNS_USE_BOTH && dns_primary == 0 && dns_secondary == 0) return DNS_ERR_NO_DNS;
    socket_handle_t sock = dns_socket_handle();
    if (sock == 0) return DNS_ERR_SOCKET;
    dns_result_t res = DNS_ERR_NO_DNS;
    if (which == DNS_USE_PRIMARY) res = perform_dns_query_once(sock, dns_primary, hostname, timeout_ms, out_ip);
    else if (which == DNS_USE_SECONDARY) res = perform_dns_query_once(sock, dns_secondary, hostname, timeout_ms, out_ip);
    else {
        res = perform_dns_query_once(sock, dns_primary ? dns_primary : dns_secondary, hostname, timeout_ms, out_ip);
        if (res != DNS_OK && dns_secondary && dns_secondary != dns_primary) res = perform_dns_query_once(sock, dns_secondary, hostname, timeout_ms, out_ip);
    }
    return res;
}
