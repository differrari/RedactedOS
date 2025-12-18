#include "dns.h"
#include "std/memory.h"
#include "dev/random/random.h"
#include "process/scheduler.h"
#include "types.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/interface_manager.h"
#include "dns_daemon.h"
#include "syscalls/syscalls.h"


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

static dns_result_t parse_dns_aaaa_record(uint8_t* buffer, uint32_t buffer_len, uint16_t message_id, uint8_t out_ipv6[16]){
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
        if (type == 28 && klass == 1 && rdlength == 16){
            memcpy(out_ipv6, buffer+offset, 16);
            return DNS_OK;
        }
        offset += rdlength;
    }
    return DNS_ERR_NO_ANSWER;
}

static dns_result_t perform_dns_query_once_a(socket_handle_t sock, uint32_t dns_ip_host, const char* name, uint32_t timeout_ms, uint32_t* out_ip){
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

    net_l4_endpoint dst = {0};
    dst.ver = IP_VER4;
    memcpy(dst.ip, &dns_ip_host, 4);
    dst.port = 53;

    int64_t sent = socket_sendto_udp_ex(sock, DST_ENDPOINT, &dst, 0, request_buffer, offset);
    if (sent < 0) return DNS_ERR_SEND;

    uint32_t waited_ms = 0;
    while (waited_ms < timeout_ms){
        uint8_t response_buffer[512];
        net_l4_endpoint source;
        int64_t received = socket_recvfrom_udp_ex(sock, response_buffer, sizeof(response_buffer), &source);
        if (received > 0 && source.port == 53 && source.ver == IP_VER4 && *(uint32_t*)source.ip == dns_ip_host){
            uint32_t ip_host;
            dns_result_t pr = parse_dns_a_record(response_buffer, (uint32_t)received, message_id, &ip_host);
            if (pr == DNS_OK){ *out_ip = ip_host; return DNS_OK; }
            if (pr == DNS_ERR_NXDOMAIN) return pr;
        } else {
            msleep(50);
            waited_ms += 50;
        }
    }
    return DNS_ERR_TIMEOUT;
}

static dns_result_t perform_dns_query_once_aaaa(socket_handle_t sock, uint32_t dns_ip_host, const char* name, uint32_t timeout_ms, uint8_t out_ipv6[16]){
    uint8_t request_buffer[512]; memset(request_buffer, 0, sizeof(request_buffer));
    rng_t rng; rng_init_random(&rng);
    uint16_t message_id = (uint16_t)(rng_next32(&rng) & 0xFFFF);
    wr_be16(request_buffer+0, message_id);
    wr_be16(request_buffer+2, 0x0100);
    wr_be16(request_buffer+4, 1);
    uint32_t offset = 12;
    offset += encode_dns_qname(request_buffer+offset, name);
    wr_be16(request_buffer+offset+0, 28);
    wr_be16(request_buffer+offset+2, 1);
    offset += 4;

    net_l4_endpoint dst = {0};
    dst.ver = IP_VER4;
    memcpy(dst.ip, &dns_ip_host, 4);
    dst.port = 53;

    int64_t sent = socket_sendto_udp_ex(sock, DST_ENDPOINT, &dst, 0, request_buffer, offset);
    if (sent < 0) return DNS_ERR_SEND;

    uint32_t waited_ms = 0;
    while (waited_ms < timeout_ms){
        uint8_t response_buffer[512];
        net_l4_endpoint source;
        int64_t received = socket_recvfrom_udp_ex(sock, response_buffer, sizeof(response_buffer), &source);
        if (received > 0 && source.port == 53 && source.ver == IP_VER4 && *(uint32_t*)source.ip == dns_ip_host){
            dns_result_t pr = parse_dns_aaaa_record(response_buffer, (uint32_t)received, message_id, out_ipv6);
            if (pr == DNS_OK) return DNS_OK;
            if (pr == DNS_ERR_NXDOMAIN) return pr;
        } else {
            msleep(50);
            waited_ms += 50;
        }
    }
    return DNS_ERR_TIMEOUT;
}

static bool pick_dns_on_l3(uint8_t l3_id, uint32_t* out_primary, uint32_t* out_secondary){
    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(l3_id);
    if (!v4) return false;
    uint32_t p = v4->runtime_opts_v4.dns[0];
    uint32_t s = v4->runtime_opts_v4.dns[1];
    if (out_primary)  *out_primary  = p;
    if (out_secondary) *out_secondary = s;
    return (p != 0) || (s != 0);
}

static bool pick_dns_first_iface(uint8_t* out_l3, uint32_t* out_primary, uint32_t* out_secondary){
    uint8_t n = l2_interface_count();
    for (uint8_t i = 0; i < n; ++i){
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2) continue;
        for (int s = 0; s < MAX_IPV4_PER_INTERFACE; ++s){
            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
            if (!v4 || v4->mode == IPV4_CFG_DISABLED) continue;

            uint32_t p = v4->runtime_opts_v4.dns[0];
            uint32_t q = v4->runtime_opts_v4.dns[1];
            if (p || q){
                if (out_l3)       *out_l3       = v4->l3_id;
                if (out_primary)  *out_primary  = p;
                if (out_secondary)*out_secondary= q;
                return true;
            }
        }
    }
    return false;
}

static dns_result_t query_with_selection_a(uint32_t primary, uint32_t secondary, dns_server_sel_t which, const char* hostname, uint32_t timeout_ms, uint32_t* out_ip){
    if (which == DNS_USE_PRIMARY && primary == 0) return DNS_ERR_NO_DNS;
    if (which == DNS_USE_SECONDARY && secondary == 0) return DNS_ERR_NO_DNS;
    if (which == DNS_USE_BOTH && primary == 0 && secondary == 0) return DNS_ERR_NO_DNS;
    socket_handle_t sock = dns_socket_handle();
    if (sock == 0) return DNS_ERR_SOCKET;
    dns_result_t res = DNS_ERR_NO_DNS;
    if (which == DNS_USE_PRIMARY) res = perform_dns_query_once_a(sock, primary, hostname, timeout_ms, out_ip);
    else if (which == DNS_USE_SECONDARY) res = perform_dns_query_once_a(sock, secondary, hostname, timeout_ms, out_ip);
    else {
        res = perform_dns_query_once_a(sock, primary ? primary : secondary, hostname, timeout_ms, out_ip);
        if (res != DNS_OK && secondary && secondary != primary) res = perform_dns_query_once_a(sock, secondary, hostname, timeout_ms, out_ip);
    }
    return res;
}

static dns_result_t query_with_selection_aaaa(uint32_t primary, uint32_t secondary, dns_server_sel_t which, const char* hostname, uint32_t timeout_ms, uint8_t out_ipv6[16]){
    if (which == DNS_USE_PRIMARY && primary == 0) return DNS_ERR_NO_DNS;
    if (which == DNS_USE_SECONDARY && secondary == 0) return DNS_ERR_NO_DNS;
    if (which == DNS_USE_BOTH && primary == 0 && secondary == 0) return DNS_ERR_NO_DNS;
    socket_handle_t sock = dns_socket_handle();
    if (sock == 0) return DNS_ERR_SOCKET;
    dns_result_t res = DNS_ERR_NO_DNS;
    if (which == DNS_USE_PRIMARY) res = perform_dns_query_once_aaaa(sock, primary, hostname, timeout_ms, out_ipv6);
    else if (which == DNS_USE_SECONDARY) res = perform_dns_query_once_aaaa(sock, secondary, hostname, timeout_ms, out_ipv6);
    else {
        res = perform_dns_query_once_aaaa(sock, primary ? primary : secondary, hostname, timeout_ms, out_ipv6);
        if (res != DNS_OK && secondary && secondary != primary) res = perform_dns_query_once_aaaa(sock, secondary, hostname, timeout_ms, out_ipv6);
    }
    return res;
}

dns_result_t dns_resolve_a(const char* hostname, uint32_t* out_ip, dns_server_sel_t which, uint32_t timeout_ms){
    uint8_t l3 = 0;
    uint32_t p = 0, s = 0;
    if (!pick_dns_first_iface(&l3, &p, &s)) return DNS_ERR_NO_DNS;
    return query_with_selection_a(p, s, which, hostname, timeout_ms, out_ip);
}

dns_result_t dns_resolve_a_on_l3(uint8_t l3_id, const char* hostname, uint32_t* out_ip, dns_server_sel_t which, uint32_t timeout_ms){
    uint32_t p = 0, s = 0;
    if (!pick_dns_on_l3(l3_id, &p, &s)) return DNS_ERR_NO_DNS;
    return query_with_selection_a(p, s, which, hostname, timeout_ms, out_ip);
}

dns_result_t dns_resolve_aaaa(const char* hostname, uint8_t out_ipv6[16], dns_server_sel_t which, uint32_t timeout_ms){
    uint8_t l3 = 0;
    uint32_t p = 0, s = 0;
    if (!pick_dns_first_iface(&l3, &p, &s)) return DNS_ERR_NO_DNS;
    return query_with_selection_aaaa(p, s, which, hostname, timeout_ms, out_ipv6);
}

dns_result_t dns_resolve_aaaa_on_l3(uint8_t l3_id, const char* hostname, uint8_t out_ipv6[16], dns_server_sel_t which, uint32_t timeout_ms){
    uint32_t p = 0, s = 0;
    if (!pick_dns_on_l3(l3_id, &p, &s)) return DNS_ERR_NO_DNS;
    return query_with_selection_aaaa(p, s, which, hostname, timeout_ms, out_ipv6);
}
