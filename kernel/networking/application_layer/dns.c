#include "dns.h"
#include "dns_mdns.h"
#include "dns_cache.h"
#include "std/std.h"
#include "math/math.h"
#include "process/scheduler.h"
#include "types.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv6_utils.h"

#include "networking/interface_manager.h"
#include "dns_daemon.h"
#include "syscalls/syscalls.h"
#include "networking/transport_layer/trans_utils.h"

#define MDNS_TIMEOUT_A_MS 500u
#define MDNS_TIMEOUT_AAAA_MS 300u

static bool dns_is_local_name(const char* hostname) {
    if (!hostname) return false;
    uint32_t nlen = strlen(hostname);
    if (nlen < 6u) return false;
    if (strncmp(hostname +(nlen - 6u), ".local", 6) != 0)return false;
    return true;
}

static dns_result_t dns_write_qname(uint8_t* buf, uint32_t buf_len, uint32_t* offset, const char* name) {
    if (!buf || !offset || !name) return DNS_ERR_FORMAT;
    uint32_t off = *offset;
    if (off >= buf_len) return DNS_ERR_FORMAT;
    uint32_t label_len = 0;
    uint32_t label_pos = off;
    buf[off++] = 0;
    for (const char* p = name; *p; ++p) {
        char c = *p;
        if (c =='.') {
            if (!label_len || label_len > 63u) return DNS_ERR_FORMAT;
            buf[label_pos] = (uint8_t)label_len;
            label_len = 0;
            label_pos = off;
            if (off >= buf_len) return DNS_ERR_FORMAT;
            buf[off++] = 0;
            continue;
        }
        if (label_len >= 63u) return DNS_ERR_FORMAT;
        if (off >= buf_len) return DNS_ERR_FORMAT;
        buf[off++] = (uint8_t)c;
        label_len++;
    }
    if (!label_len || label_len > 63u) return DNS_ERR_FORMAT;
    buf[label_pos] = (uint8_t)label_len;
    if (off >= buf_len) return DNS_ERR_FORMAT;
    buf[off++]= 0;
    *offset = off;
    return DNS_OK;
}

static uint32_t skip_dns_name(const uint8_t* message, uint32_t message_len, uint32_t offset){
    if (offset >= message_len) return message_len + 1;
    uint32_t cursor = offset;
    while (cursor < message_len) {
        uint8_t len = message[cursor++];
        if (len == 0) break;
        if ((len & 0xC0) == 0xC0) {
            if (cursor >= message_len) return message_len + 1;
            cursor++;
            break;
        }
        cursor += len;
        if (cursor > message_len) return message_len + 1;
    }
    return cursor;
}

static dns_result_t parse_dns_a_record(uint8_t* buffer, uint32_t buffer_len, uint16_t message_id, uint32_t* out_ip, uint32_t* out_ttl_s){
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
        uint32_t ttl_s = rd_be32(buffer+offset+4);
        uint16_t rdlength = rd_be16(buffer+offset+8);
        offset += 10;
        if (offset + rdlength > buffer_len) return DNS_ERR_FORMAT;
        if (type == 1 && klass == 1 && rdlength == 4){
            uint32_t ip_host = rd_be32(buffer+offset);
            *out_ip = ip_host;
            if (out_ttl_s) *out_ttl_s = ttl_s;
            return DNS_OK;
        }
        offset += rdlength;
    }
    return DNS_ERR_NO_ANSWER;
}

static dns_result_t parse_dns_aaaa_record(uint8_t* buffer, uint32_t buffer_len, uint16_t message_id, uint8_t out_ipv6[16], uint32_t* out_ttl_s){
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
        uint32_t ttl_s = rd_be32(buffer+offset+4);
        uint16_t rdlength = rd_be16(buffer+offset+8);
        offset += 10;
        if (offset + rdlength > buffer_len) return DNS_ERR_FORMAT;
        if (type == 28 && klass == 1 && rdlength == 16){
            memcpy(out_ipv6, buffer+offset, 16);
            if (out_ttl_s) *out_ttl_s = ttl_s;
            return DNS_OK;
        }
        offset += rdlength;
    }
    return DNS_ERR_NO_ANSWER;
}

static dns_result_t perform_dns_query_once_a(socket_handle_t sock, const net_l4_endpoint* dns_srv, const char* name, uint32_t timeout_ms, uint32_t* out_ip, uint32_t* out_ttl_s){
    uint8_t request_buffer[512];
    memset(request_buffer,0,sizeof(request_buffer));
    rng_t rng;
    uint64_t virt_timer;
    asm volatile ("mrs %0, cntvct_el0" : "=r"(virt_timer));
    rng_seed(&rng, virt_timer);
    uint16_t message_id = (uint16_t)(rng_next32(&rng) & 0xFFFF);
    wr_be16(request_buffer+0, message_id);
    wr_be16(request_buffer+2, 0x0100);
    wr_be16(request_buffer+4, 1);
    uint32_t offset = 12;
    dns_result_t qnr=dns_write_qname(request_buffer, sizeof(request_buffer), &offset, name);
    if (qnr != DNS_OK) return qnr;

    if (offset+ 4 > sizeof(request_buffer)) return DNS_ERR_FORMAT;

    wr_be16(request_buffer+offset+0, 1);
    wr_be16(request_buffer+offset+2, 1);
    offset += 4;

    net_l4_endpoint dst = *dns_srv;
    dst.port = 53;

    int64_t sent = socket_sendto_udp_ex(sock, DST_ENDPOINT, &dst, 0, request_buffer, offset);
    if (sent < 0) return DNS_ERR_SEND;

    uint32_t waited_ms = 0;
    while (waited_ms < timeout_ms){
        uint8_t response_buffer[512];
        net_l4_endpoint source;
        int64_t received = socket_recvfrom_udp_ex(sock, response_buffer, sizeof(response_buffer), &source);
        bool ok_src = false;
        if (received > 0 && source.port == 53 && source.ver == dst.ver) {
            if (dst.ver == IP_VER4) ok_src = (*(uint32_t*)source.ip == *(uint32_t*)dst.ip);
            else if (dst.ver == IP_VER6) ok_src = (memcmp(source.ip, dst.ip, 16) == 0);
        }
        if (ok_src){
            uint32_t ip_host;
            uint32_t ttl_s = 0;
            dns_result_t pr = parse_dns_a_record(response_buffer, (uint32_t)received, message_id, &ip_host, &ttl_s);
            if(pr == DNS_OK){
                *out_ip = ip_host;
                if(out_ttl_s) *out_ttl_s = ttl_s;
                return DNS_OK;
            }
            if (pr == DNS_ERR_NXDOMAIN) return pr;
        }
        msleep(50);
        waited_ms += 50;
    }
    return DNS_ERR_TIMEOUT;
}

static dns_result_t perform_dns_query_once_aaaa(socket_handle_t sock, const net_l4_endpoint* dns_srv, const char* name, uint32_t timeout_ms, uint8_t out_ipv6[16], uint32_t* out_ttl_s){
    uint8_t request_buffer[512];
    memset(request_buffer, 0, sizeof(request_buffer));
    rng_t rng;
    uint64_t virt_timer;
    asm volatile ("mrs %0, cntvct_el0" : "=r"(virt_timer));
    rng_seed(&rng, virt_timer);
    uint16_t message_id = (uint16_t)(rng_next32(&rng) & 0xFFFF);
    wr_be16(request_buffer+0, message_id);
    wr_be16(request_buffer+2, 0x0100);
    wr_be16(request_buffer+4, 1);
    uint32_t offset = 12;
    dns_result_t qnr=dns_write_qname(request_buffer, sizeof(request_buffer), &offset, name);
    if (qnr != DNS_OK) return qnr;

    if (offset+ 4 > sizeof(request_buffer)) return DNS_ERR_FORMAT;

    wr_be16(request_buffer+offset+0, 28);
    wr_be16(request_buffer+offset+2, 1);
    offset += 4;

    net_l4_endpoint dst = *dns_srv;
    dst.port = 53;

    int64_t sent = socket_sendto_udp_ex(sock, DST_ENDPOINT, &dst, 0, request_buffer, offset);
    if (sent < 0) return DNS_ERR_SEND;

    uint32_t waited_ms = 0;
    while (waited_ms < timeout_ms){
        uint8_t response_buffer[512];
        net_l4_endpoint source;
        int64_t received = socket_recvfrom_udp_ex(sock, response_buffer, sizeof(response_buffer), &source);
        bool ok_src = false;
        if (received > 0 && source.port == 53 && source.ver == dst.ver) {
            if (dst.ver == IP_VER4) ok_src = (*(uint32_t*)source.ip == *(uint32_t*)dst.ip);
            else if (dst.ver == IP_VER6) ok_src = (memcmp(source.ip, dst.ip, 16) == 0);
        }
        if (ok_src){
            uint32_t ttl_s = 0;
            dns_result_t pr = parse_dns_aaaa_record(response_buffer, (uint32_t)received, message_id, out_ipv6, &ttl_s);
            if (pr == DNS_OK){
                if (out_ttl_s) *out_ttl_s = ttl_s;
                return DNS_OK;
            }
            if (pr == DNS_ERR_NXDOMAIN) return pr;
        } else {
            msleep(50);
            waited_ms += 50;
        }
    }
    return DNS_ERR_TIMEOUT;
}

static bool pick_dns_on_l3(uint8_t l3_id, net_l4_endpoint* out_primary, net_l4_endpoint* out_secondary){
    if (l3_ipv4_find_by_id(l3_id)) {
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(l3_id);
        if (!v4) return false;
        uint32_t p = v4->runtime_opts_v4.dns[0];
        uint32_t s = v4->runtime_opts_v4.dns[1];
        if (out_primary) {
            memset(out_primary, 0, sizeof(*out_primary));
            out_primary->ver = IP_VER4;
            memcpy(out_primary->ip, &p, 4);
        }
        if (out_secondary) {
            memset(out_secondary, 0, sizeof(*out_secondary));
            out_secondary->ver = IP_VER4;
            memcpy(out_secondary->ip, &s, 4);
        }
        return (p != 0) || (s != 0);
    }

    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(l3_id);
    if (!v6) return false;
    static const uint8_t z[16] = {0};
    const uint8_t* p6 = v6->runtime_opts_v6.dns[0];
    const uint8_t* s6 = v6->runtime_opts_v6.dns[1];
    bool hp = memcmp(p6, z, 16) != 0;
    bool hs = memcmp(s6, z, 16) != 0;
    if (out_primary) {
        memset(out_primary, 0, sizeof(*out_primary));
        out_primary->ver = IP_VER6;
        if (hp) memcpy(out_primary->ip, p6, 16);
    }
    if (out_secondary) {
        memset(out_secondary, 0, sizeof(*out_secondary));
        out_secondary->ver = IP_VER6;
        if (hs) memcpy(out_secondary->ip, s6, 16);
    }
    return hp || hs;
}

static bool pick_dns_first_iface(uint8_t* out_l3, net_l4_endpoint* out_primary, net_l4_endpoint* out_secondary){
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
                if (out_l3) *out_l3 = v4->l3_id;
                if (out_primary) {
                    memset(out_primary, 0, sizeof(*out_primary));
                    out_primary->ver = IP_VER4;
                    memcpy(out_primary->ip, &p, 4);
                }
                if (out_secondary) {
                    memset(out_secondary, 0, sizeof(*out_secondary));
                    out_secondary->ver = IP_VER4;
                    memcpy(out_secondary->ip, &q, 4);
                }
                return true;
            }
        }

        for (int s = 0; s < MAX_IPV6_PER_INTERFACE; ++s) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[s];
            if (!v6 || v6->cfg == IPV6_CFG_DISABLE) continue;
            static const uint8_t z[16] = {0};
            bool hp = memcmp(v6->runtime_opts_v6.dns[0], z, 16) != 0;
            bool hq = memcmp(v6->runtime_opts_v6.dns[1], z, 16) != 0;
            if (hp || hq){
                if (out_l3) *out_l3 = v6->l3_id;
                if (out_primary) {
                    memset(out_primary, 0, sizeof(*out_primary));
                    out_primary->ver = IP_VER6;
                    if (hp) memcpy(out_primary->ip, v6->runtime_opts_v6.dns[0], 16);
                }
                if (out_secondary) {
                    memset(out_secondary, 0, sizeof(*out_secondary));
                    out_secondary->ver = IP_VER6;
                    if (hq) memcpy(out_secondary->ip, v6->runtime_opts_v6.dns[1], 16);
                }
                return true;
            }
        }
    }
    return false;
}

static bool dns_srv_is_zero(const net_l4_endpoint* e){
    if (!e) return true;
    if (e->ver == IP_VER4) return *(const uint32_t*)e->ip == 0;
    if (e->ver == IP_VER6) return ipv6_is_unspecified(e->ip);
    return true;
}

static dns_result_t query_with_selection_a(const net_l4_endpoint* primary, const net_l4_endpoint* secondary, dns_server_sel_t which, const char* hostname, uint32_t timeout_ms, uint32_t* out_ip){
    if (which == DNS_USE_PRIMARY && dns_srv_is_zero(primary)) return DNS_ERR_NO_DNS;
    if (which == DNS_USE_SECONDARY && dns_srv_is_zero(secondary)) return DNS_ERR_NO_DNS;
    if (which == DNS_USE_BOTH && dns_srv_is_zero(primary) && dns_srv_is_zero(secondary)) return DNS_ERR_NO_DNS;
    socket_handle_t sock = dns_socket_handle();
    if (sock == 0) return DNS_ERR_SOCKET;
    dns_result_t res = DNS_ERR_NO_DNS;
    uint32_t ttl_s = 0;
    if (which == DNS_USE_PRIMARY) res = perform_dns_query_once_a(sock, primary, hostname, timeout_ms, out_ip, &ttl_s);
    else if (which == DNS_USE_SECONDARY) res = perform_dns_query_once_a(sock, secondary, hostname, timeout_ms, out_ip, &ttl_s);
    else {
        const net_l4_endpoint* first = !dns_srv_is_zero(primary) ? primary : secondary;
        const net_l4_endpoint* second = !dns_srv_is_zero(secondary) ? secondary : primary;
        res = perform_dns_query_once_a(sock, first, hostname, timeout_ms, out_ip, &ttl_s);
        if (res != DNS_OK && second && first != second) res = perform_dns_query_once_a(sock, second, hostname, timeout_ms, out_ip, &ttl_s);
    }
    if (res == DNS_OK) {
        uint32_t ttl_ms = ttl_s > (0xFFFFFFFFu / 1000u) ? 0xFFFFFFFFu : ttl_s * 1000u;
        uint8_t addr[16];
        memset(addr, 0, 16);
        wr_be32(addr, *out_ip);
        dns_cache_put_ip(hostname, 1, addr, ttl_ms);
    }
    return res;
}

static dns_result_t query_with_selection_aaaa(const net_l4_endpoint* primary, const net_l4_endpoint* secondary, dns_server_sel_t which, const char* hostname, uint32_t timeout_ms, uint8_t out_ipv6[16]){
    if (which == DNS_USE_PRIMARY && dns_srv_is_zero(primary)) return DNS_ERR_NO_DNS;
    if (which == DNS_USE_SECONDARY && dns_srv_is_zero(secondary)) return DNS_ERR_NO_DNS;
    if (which == DNS_USE_BOTH && dns_srv_is_zero(primary) && dns_srv_is_zero(secondary)) return DNS_ERR_NO_DNS;
    socket_handle_t sock = dns_socket_handle();
    if (sock == 0) return DNS_ERR_SOCKET;
    dns_result_t res = DNS_ERR_NO_DNS;
    uint32_t ttl_s = 0;
    if (which == DNS_USE_PRIMARY) res = perform_dns_query_once_aaaa(sock, primary, hostname, timeout_ms, out_ipv6, &ttl_s);
    else if (which == DNS_USE_SECONDARY) res = perform_dns_query_once_aaaa(sock, secondary, hostname, timeout_ms, out_ipv6, &ttl_s);
    else {
        const net_l4_endpoint* first = !dns_srv_is_zero(primary) ? primary : secondary;
        const net_l4_endpoint* second = !dns_srv_is_zero(secondary) ? secondary : primary;
        res = perform_dns_query_once_aaaa(sock, first, hostname, timeout_ms, out_ipv6, &ttl_s);
        if (res != DNS_OK && second && first != second) res = perform_dns_query_once_aaaa(sock, second, hostname, timeout_ms, out_ipv6, &ttl_s);
    }
    if (res == DNS_OK) {
        uint32_t ttl_ms = ttl_s > (0xFFFFFFFFu / 1000u) ? 0xFFFFFFFFu : ttl_s * 1000u;
        dns_cache_put_ip(hostname, 28, out_ipv6, ttl_ms);
    }
    return res;
}

dns_result_t dns_resolve_a(const char* hostname, uint32_t* out_ip, dns_server_sel_t which, uint32_t timeout_ms){
    if (!hostname || !out_ip) return DNS_ERR_FORMAT;
    uint8_t cached[16];
    if (dns_cache_get_ip(hostname, 1, cached)) {
        *out_ip = rd_be32(cached);
        return DNS_OK;
    }

    bool is_local = dns_is_local_name(hostname);

    if (is_local){
        uint32_t ttl_s = 0;
        dns_result_t mr = mdns_resolve_a(hostname, timeout_ms > MDNS_TIMEOUT_A_MS ? MDNS_TIMEOUT_A_MS : timeout_ms, out_ip, &ttl_s);
        if (mr == DNS_OK) {
            uint8_t a[16];
            memset(a, 0, 16);
            wr_be32(a, *out_ip);
            uint32_t ttl_ms = ttl_s > (0xFFFFFFFFu / 1000u) ? 0xFFFFFFFFu : ttl_s * 1000u;
            dns_cache_put_ip(hostname, 1, a, ttl_ms);
        }
        return mr;
    }

    dns_result_t res = DNS_ERR_NO_DNS;
    uint8_t l3 = 0;
    net_l4_endpoint p, s;
    if (pick_dns_first_iface(&l3, &p, &s)) res = query_with_selection_a(&p, &s, which, hostname, timeout_ms, out_ip);

    if (res != DNS_OK && is_local){
        uint32_t ttl_s = 0;
        dns_result_t mr = mdns_resolve_a(hostname, timeout_ms > MDNS_TIMEOUT_A_MS ? MDNS_TIMEOUT_A_MS : timeout_ms, out_ip, &ttl_s);
        if (mr == DNS_OK) {
            uint8_t a[16];
            memset(a,0, 16);
            wr_be32(a, *out_ip);
            uint32_t ttl_ms = ttl_s > (0xFFFFFFFFu / 1000u) ? 0xFFFFFFFFu : ttl_s * 1000u;
            dns_cache_put_ip(hostname, 1, a, ttl_ms);
            return DNS_OK;
        }
    }

    return res;
}

dns_result_t dns_resolve_a_on_l3(uint8_t l3_id, const char* hostname, uint32_t* out_ip, dns_server_sel_t which, uint32_t timeout_ms){
    if (!hostname || !out_ip) return DNS_ERR_FORMAT;
    uint8_t cached[16];
    if (dns_cache_get_ip(hostname, 1, cached)) {
        *out_ip = rd_be32(cached);
        return DNS_OK;
    }

    bool is_local = dns_is_local_name(hostname);

    if (is_local) {
        uint32_t ttl_s = 0;
        dns_result_t mr = mdns_resolve_a(hostname, timeout_ms > MDNS_TIMEOUT_A_MS ? MDNS_TIMEOUT_A_MS : timeout_ms, out_ip, &ttl_s);
        if (mr == DNS_OK) {
            uint8_t a[16];
            memset(a, 0, 16);
            wr_be32(a, *out_ip);
            uint32_t ttl_ms = ttl_s > (0xFFFFFFFFu / 1000u) ? 0xFFFFFFFFu : ttl_s * 1000u;
            dns_cache_put_ip(hostname, 1, a, ttl_ms);
        }
        return mr;
    }

    dns_result_t res = DNS_ERR_NO_DNS;
    net_l4_endpoint p, s;

    if (pick_dns_on_l3(l3_id, &p, &s)) res = query_with_selection_a(&p, &s, which, hostname, timeout_ms, out_ip);

    if (res != DNS_OK && is_local) {
        uint32_t ttl_s = 0;
        dns_result_t mr = mdns_resolve_a(hostname, timeout_ms > MDNS_TIMEOUT_A_MS ? MDNS_TIMEOUT_A_MS : timeout_ms, out_ip, &ttl_s);
        if (mr == DNS_OK) {
            uint8_t a[16];
            memset(a, 0, 16);
            wr_be32(a, *out_ip);
            uint32_t ttl_ms = ttl_s > (0xFFFFFFFFu / 1000u) ? 0xFFFFFFFFu : ttl_s * 1000u;
            dns_cache_put_ip(hostname, 1, a, ttl_ms);
            return DNS_OK;
        }
    }

    return res;
}

dns_result_t dns_resolve_aaaa(const char* hostname, uint8_t out_ipv6[16], dns_server_sel_t which, uint32_t timeout_ms){
    if (!hostname || !out_ipv6) return DNS_ERR_FORMAT;
    if (dns_cache_get_ip(hostname, 28, out_ipv6)) return DNS_OK;

    bool is_local = dns_is_local_name(hostname);

    if (is_local) {
        uint32_t ttl_s = 0;
        dns_result_t mr = mdns_resolve_aaaa(hostname, timeout_ms > MDNS_TIMEOUT_AAAA_MS ? MDNS_TIMEOUT_AAAA_MS : timeout_ms, out_ipv6, &ttl_s);
        if (mr == DNS_OK) {
            uint32_t ttl_ms = ttl_s > (0xFFFFFFFFu / 1000u) ? 0xFFFFFFFFu : ttl_s * 1000u;
            dns_cache_put_ip(hostname, 28, out_ipv6, ttl_ms);
        }
        return mr;
    }

    dns_result_t res = DNS_ERR_NO_DNS;
    uint8_t l3 = 0;
    net_l4_endpoint p, s;
    if (pick_dns_first_iface(&l3, &p, &s)) res = query_with_selection_aaaa(&p, &s, which, hostname, timeout_ms, out_ipv6);

    if (res != DNS_OK && is_local) {
        uint32_t ttl_s = 0;
        dns_result_t mr = mdns_resolve_aaaa(hostname, timeout_ms > MDNS_TIMEOUT_AAAA_MS ? MDNS_TIMEOUT_AAAA_MS : timeout_ms, out_ipv6, &ttl_s);
        if (mr == DNS_OK) {
            uint32_t ttl_ms = ttl_s > (0xFFFFFFFFu / 1000u) ? 0xFFFFFFFFu : ttl_s * 1000u;
            dns_cache_put_ip(hostname, 28, out_ipv6, ttl_ms);
            return DNS_OK;
        }
    }

    return res;
}

dns_result_t dns_resolve_aaaa_on_l3(uint8_t l3_id, const char* hostname, uint8_t out_ipv6[16], dns_server_sel_t which, uint32_t timeout_ms){
    if (!hostname || !out_ipv6) return DNS_ERR_FORMAT;
    if (dns_cache_get_ip(hostname, 28, out_ipv6)) return DNS_OK;
    bool is_local = dns_is_local_name(hostname);
    if (is_local) {
        uint32_t ttl_s = 0;
        dns_result_t mr = mdns_resolve_aaaa(hostname, timeout_ms > MDNS_TIMEOUT_AAAA_MS ? MDNS_TIMEOUT_AAAA_MS : timeout_ms, out_ipv6, &ttl_s);
        if (mr == DNS_OK) {
            uint32_t ttl_ms = ttl_s > (0xFFFFFFFFu / 1000u) ? 0xFFFFFFFFu : ttl_s * 1000u;
            dns_cache_put_ip(hostname, 28, out_ipv6, ttl_ms);
        }
        return mr;
    }

    dns_result_t res = DNS_ERR_NO_DNS;
    net_l4_endpoint p, s;
    if (pick_dns_on_l3(l3_id, &p, &s)) res = query_with_selection_aaaa(&p, &s, which, hostname, timeout_ms, out_ipv6);

    if (res != DNS_OK && is_local) {
        uint32_t ttl_s = 0;
        dns_result_t mr = mdns_resolve_aaaa(hostname, timeout_ms > MDNS_TIMEOUT_AAAA_MS ? MDNS_TIMEOUT_AAAA_MS : timeout_ms, out_ipv6, &ttl_s);
        if (mr == DNS_OK) {
            uint32_t ttl_ms = ttl_s > (0xFFFFFFFFu / 1000u) ? 0xFFFFFFFFu : ttl_s * 1000u;
            dns_cache_put_ip(hostname, 28, out_ipv6, ttl_ms);
            return DNS_OK;
        }
    }

    return res;
}
