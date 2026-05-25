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
#define DNS_MAX_CNAME_DEPTH 4
#define DNS_QUERY_RECORDS 8

static dns_result_t perform_dns_query_once(socket_handle_t sock, const net_l4_endpoint *dns_srv, const char *name, dns_qtype_t qtype, uint32_t timeout_ms, dns_record_t *out_records, uint32_t max_records, uint32_t *out_count) {
    if (out_count) *out_count = 0;
    if (!sock) return DNS_ERR_SOCKET;
    if (!dns_srv) return DNS_ERR_NO_DNS;
    if (!name) return DNS_ERR_FORMAT;
    if (!out_records && max_records) return DNS_ERR_FORMAT;

    uint8_t request_buffer[512];
    rng_t rng;
    uint64_t virt_timer;
    asm volatile ("mrs %0, cntvct_el0" : "=r"(virt_timer));
    rng_seed(&rng, virt_timer);
    uint16_t message_id = (uint16_t)(rng_next32(&rng) & 0xFFFF);
    uint32_t request_len = dns_wire_build_query(request_buffer, sizeof(request_buffer), message_id, name, qtype, false);
    if (!request_len) return DNS_ERR_FORMAT;

    net_l4_endpoint dst = *dns_srv;
    dst.port = 53;

    int64_t sent = socket_sendto_udp(sock, DST_ENDPOINT, &dst, 0, request_buffer, request_len);
    if (sent < 0) return DNS_ERR_SEND;

    uint32_t waited_ms = 0;
    while (waited_ms < timeout_ms){
        uint8_t response_buffer[512];
        net_l4_endpoint source;
        int64_t received = socket_recvfrom_udp(sock, response_buffer, sizeof(response_buffer), &source);
        if (received > 0 && source.port == 53 && source.ver == dst.ver && ((dst.ver == IP_VER4 && memcmp(source.ip, dst.ip, 4) == 0) || (dst.ver == IP_VER6 && memcmp(source.ip, dst.ip, 16) == 0))) {
            uint32_t received_len = received;
            dns_record_t parsed[DNS_QUERY_RECORDS];
            uint32_t parsed_count = 0;
            uint16_t flags = 0;
            if (!dns_wire_parse_records(response_buffer, received_len, true, message_id, parsed, DNS_QUERY_RECORDS, &parsed_count, &flags)) {
                msleep(50);
                waited_ms += 50;
                continue;
            }
            if ((flags & DNS_RCODE_MASK) == DNS_RCODE_NXDOMAIN) return DNS_ERR_NXDOMAIN;

            uint32_t count = 0;
            for (uint32_t i = 0; i < parsed_count; i++) {
                if ((parsed[i].rrclass & DNS_CLASS_MASK) != DNS_CLASS_IN) continue;
                if (qtype != DNS_TYPE_ANY && parsed[i].type != qtype && parsed[i].type != DNS_TYPE_CNAME) continue;
                if (count < max_records) out_records[count] = parsed[i];
                count++;
            }

            if (count) {
                if (out_count) *out_count = count < max_records ? count : max_records;
                return DNS_OK;
            }
            return DNS_ERR_NO_ANSWER;
        }
        msleep(50);
        waited_ms += 50;
    }
    return DNS_ERR_TIMEOUT;
}

static bool pick_dns_on_l3(uint8_t l3_id, net_l4_endpoint* out_primary, net_l4_endpoint* out_secondary) {
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
        return p || s;
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
            bool hp = !ipv6_is_unspecified(v6->runtime_opts_v6.dns[0]);
            bool hq = !ipv6_is_unspecified(v6->runtime_opts_v6.dns[1]);
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
    if (e->ver == IP_VER4) return rd_be32(e->ip) == 0;
    if (e->ver == IP_VER6) return ipv6_is_unspecified(e->ip);
    return true;
}

static dns_result_t query_with_selection(const net_l4_endpoint* primary, const net_l4_endpoint* secondary, dns_server_sel_t which, const char* hostname, dns_qtype_t qtype, uint32_t timeout_ms, dns_record_t* out_records, uint32_t max_records, uint32_t* out_count) {
    if (out_count) *out_count = 0;
    if (which == DNS_USE_PRIMARY && dns_srv_is_zero(primary)) return DNS_ERR_NO_DNS;
    if (which == DNS_USE_SECONDARY && dns_srv_is_zero(secondary)) return DNS_ERR_NO_DNS;
    if (which == DNS_USE_BOTH && dns_srv_is_zero(primary) && dns_srv_is_zero(secondary)) return DNS_ERR_NO_DNS;
    socket_handle_t sock = dns_socket_handle();
    if (sock == 0) return DNS_ERR_SOCKET;

    if (which == DNS_USE_PRIMARY) return perform_dns_query_once(sock, primary, hostname, qtype, timeout_ms, out_records, max_records, out_count);
    if (which == DNS_USE_SECONDARY) return perform_dns_query_once(sock, secondary, hostname, qtype, timeout_ms, out_records, max_records, out_count);

    const net_l4_endpoint* first = !dns_srv_is_zero(primary) ? primary : secondary;
    const net_l4_endpoint* second = !dns_srv_is_zero(secondary) ? secondary : primary;

    dns_result_t res = perform_dns_query_once(sock, first, hostname, qtype, timeout_ms, out_records, max_records, out_count);
    if (res != DNS_OK && second && first != second) res = perform_dns_query_once(sock, second, hostname, qtype, timeout_ms, out_records, max_records, out_count);
    return res;
}

dns_result_t dns_query(const char* hostname, dns_qtype_t qtype, dns_record_t* out_records, uint32_t max_records, uint32_t* out_count, dns_server_sel_t which, uint32_t timeout_ms) {
    if (out_count) *out_count = 0;
    if (!hostname) return DNS_ERR_FORMAT;
    if (!out_records && max_records) return DNS_ERR_FORMAT;
    if (dns_wire_is_local_name(hostname)) return mdns_query(hostname, qtype, timeout_ms, out_records, max_records, out_count);

    dns_result_t res = DNS_ERR_NO_DNS;
    uint8_t l3 = 0;
    net_l4_endpoint p, s;
    if (pick_dns_first_iface(&l3, &p, &s)) res = query_with_selection(&p, &s, which, hostname, qtype, timeout_ms, out_records, max_records, out_count);
    return res;
}

dns_result_t dns_query_on_l3(uint8_t l3_id, const char* hostname, dns_qtype_t qtype, dns_record_t* out_records, uint32_t max_records, uint32_t* out_count, dns_server_sel_t which, uint32_t timeout_ms) {
    if (out_count) *out_count = 0;
    if (!hostname) return DNS_ERR_FORMAT;
    if (!out_records && max_records) return DNS_ERR_FORMAT;
    if (dns_wire_is_local_name(hostname))return mdns_query(hostname, qtype, timeout_ms, out_records, max_records, out_count);
    dns_result_t res = DNS_ERR_NO_DNS;
    net_l4_endpoint p,s;
    if (pick_dns_on_l3(l3_id, &p, &s)) res = query_with_selection(&p, &s, which, hostname, qtype, timeout_ms, out_records, max_records, out_count);
    return res;
}

static dns_result_t dns_resolve_ip_common(uint8_t use_l3, uint8_t l3_id, const char* hostname, dns_qtype_t qtype, uint8_t out_addr[16], dns_server_sel_t which, uint32_t timeout_ms, uint32_t *out_ttl_s) {
    if (!hostname || !out_addr) return DNS_ERR_FORMAT;

    char current[DNS_WIRE_MAX_NAME];
    if (!dns_wire_name_normalize(hostname, current, sizeof(current))) return DNS_ERR_FORMAT;

    for (uint32_t depth = 0; depth <= DNS_MAX_CNAME_DEPTH; depth++) {
        dns_record_t records[DNS_QUERY_RECORDS];
        uint32_t count = 0;
        dns_result_t res;
        if (use_l3) res = dns_query_on_l3(l3_id, current, qtype, records, DNS_QUERY_RECORDS, &count, which, timeout_ms);
        else res = dns_query(current, qtype, records, DNS_QUERY_RECORDS, &count, which, timeout_ms);
        if (res != DNS_OK) return res;

        char cname[DNS_WIRE_MAX_NAME];
        cname[0] = 0;

        for (uint32_t i = 0; i < count; i++) {
            if ((records[i].rrclass & DNS_CLASS_MASK) != DNS_CLASS_IN) continue;
            if (records[i].type == qtype) {
                if (qtype == DNS_TYPE_A) memcpy(out_addr, records[i].addr, 4);
                else memcpy(out_addr, records[i].addr, 16);

                uint32_t ttl_ms = 0xFFFFFFFF;
                if (records[i].ttl_s <= 0xFFFFFFFFU / 1000) ttl_ms = records[i].ttl_s * 1000;
                dns_cache_put_ip(current, qtype, out_addr, ttl_ms);
                dns_cache_put_ip(hostname, qtype, out_addr, ttl_ms);
                if (out_ttl_s) *out_ttl_s = records[i].ttl_s;
                return DNS_OK;
            }
            if (records[i].type == DNS_TYPE_CNAME && records[i].target[0] && !cname[0]) strncpy(cname, records[i].target, sizeof(cname));
        }

        if (!cname[0]) return DNS_ERR_NO_ANSWER;
        if (dns_wire_name_equals(cname, current)) return DNS_ERR_FORMAT;
        strncpy(current, cname, sizeof(current));
    }

    return DNS_ERR_NO_ANSWER;
}

dns_result_t dns_resolve_a(const char* hostname, uint32_t* out_ip, dns_server_sel_t which, uint32_t timeout_ms) {
    if (!hostname || !out_ip) return DNS_ERR_FORMAT;
    uint8_t cached[16];
    if (dns_cache_get_ip(hostname, DNS_TYPE_A, cached)) {
        *out_ip = rd_be32(cached);
        return DNS_OK;
    }

    uint8_t addr[16];
    uint32_t ttl_s = 0;
    uint32_t mdns_timeout = timeout_ms > MDNS_TIMEOUT_A_MS ? MDNS_TIMEOUT_A_MS : timeout_ms;
    dns_result_t res;
    if (dns_wire_is_local_name(hostname)) res = dns_resolve_ip_common(0,0, hostname, DNS_TYPE_A, addr, which, mdns_timeout, &ttl_s);
    else res = dns_resolve_ip_common(0, 0, hostname, DNS_TYPE_A, addr, which, timeout_ms, &ttl_s);
    if (res != DNS_OK) return res;

    *out_ip = rd_be32(addr);
    return DNS_OK;
}

dns_result_t dns_resolve_a_on_l3(uint8_t l3_id, const char* hostname, uint32_t* out_ip, dns_server_sel_t which, uint32_t timeout_ms) {
    if (!hostname || !out_ip) return DNS_ERR_FORMAT;
    uint8_t cached[16];
    if (dns_cache_get_ip(hostname, DNS_TYPE_A, cached)) {
        *out_ip = rd_be32(cached);
        return DNS_OK;
    }

    uint8_t addr[16];
    uint32_t ttl_s = 0;
    uint32_t mdns_timeout = timeout_ms > MDNS_TIMEOUT_A_MS ? MDNS_TIMEOUT_A_MS : timeout_ms;
    dns_result_t res;
    if (dns_wire_is_local_name(hostname)) res = dns_resolve_ip_common(1, l3_id, hostname, DNS_TYPE_A, addr, which, mdns_timeout, &ttl_s);
    else res = dns_resolve_ip_common(1, l3_id, hostname, DNS_TYPE_A, addr, which, timeout_ms, &ttl_s);
    if (res != DNS_OK) return res;

    *out_ip = rd_be32(addr);
    return DNS_OK;
}

dns_result_t dns_resolve_aaaa(const char* hostname, uint8_t out_ipv6[16], dns_server_sel_t which, uint32_t timeout_ms) {
    if (!hostname || !out_ipv6) return DNS_ERR_FORMAT;
    if (dns_cache_get_ip(hostname, DNS_TYPE_AAAA, out_ipv6)) return DNS_OK;

    uint32_t ttl_s = 0;
    uint32_t mdns_timeout = timeout_ms > MDNS_TIMEOUT_AAAA_MS ? MDNS_TIMEOUT_AAAA_MS : timeout_ms;
    if (dns_wire_is_local_name(hostname)) return dns_resolve_ip_common(0, 0, hostname, DNS_TYPE_AAAA, out_ipv6, which, mdns_timeout, &ttl_s);
    return dns_resolve_ip_common(0, 0, hostname, DNS_TYPE_AAAA, out_ipv6, which, timeout_ms, &ttl_s);
}

dns_result_t dns_resolve_aaaa_on_l3(uint8_t l3_id, const char* hostname, uint8_t out_ipv6[16], dns_server_sel_t which, uint32_t timeout_ms) {
    if (!hostname || !out_ipv6) return DNS_ERR_FORMAT;
    if (dns_cache_get_ip(hostname, DNS_TYPE_AAAA, out_ipv6)) return DNS_OK;
    uint32_t ttl_s = 0;
    uint32_t mdns_timeout = timeout_ms > MDNS_TIMEOUT_AAAA_MS ? MDNS_TIMEOUT_AAAA_MS : timeout_ms;
    if (dns_wire_is_local_name(hostname)) return dns_resolve_ip_common(1, l3_id, hostname, DNS_TYPE_AAAA, out_ipv6, which, mdns_timeout, &ttl_s);
    return dns_resolve_ip_common(1, l3_id, hostname, DNS_TYPE_AAAA, out_ipv6, which, timeout_ms, &ttl_s);
}
