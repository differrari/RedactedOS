#include "dns.h"
#include "dns_mdns.h"
#include "dns_cache.h"
#include "std/std.h"
#include "math/math.h"
#include "process/scheduler.h"
#include "types.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"

#include "networking/interface_manager.h"
#include "syscalls/syscalls.h"
#include "networking/transport_layer/trans_utils.h"
#include "random/random.h"

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
    rng_init_random(&rng);
    uint16_t message_id = (uint16_t)(rng_next32(&rng) & 0xFFFF);
    uint32_t request_len = dns_wire_build_query(request_buffer, sizeof(request_buffer), message_id, name, qtype, false);
    if (!request_len) return DNS_ERR_FORMAT;

    net_l4_endpoint dst = *dns_srv;
    dst.port = 53;

    int64_t sent = send_to_socket(sock, &dst, (void*)request_buffer, request_len);
    if (sent < 0) return DNS_ERR_SEND;

    uint32_t waited_ms = 0;
    while (waited_ms < timeout_ms){
        uint8_t response_buffer[512];
        net_l4_endpoint source;
        int64_t received = receive_from_socket(sock, response_buffer, sizeof(response_buffer), &source);
        if (received > 0 && source.port == 53 && source.ver == dst.ver) {
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

static bool pick_dns(l3_id_t l3_id, net_l4_endpoint* out_primary, net_l4_endpoint* out_secondary) {
    if (l3_id) {
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(l3_id);
        if (v4) {
            uint32_t primary = v4->runtime_opts_v4.dns[0];
            uint32_t secondary = v4->runtime_opts_v4.dns[1];
            if (out_primary) make_ep(&primary, 0, IP_VER4, out_primary);
            if (out_secondary) make_ep(&secondary, 0, IP_VER4, out_secondary);
            return primary || secondary;
        }

        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(l3_id);
        if (!v6) return false;
        bool have_primary = !ipv6_is_unspecified(v6->runtime_opts_v6.dns[0]);
        bool have_secondary = !ipv6_is_unspecified(v6->runtime_opts_v6.dns[1]);
        if (out_primary) make_ep(have_primary ? v6->runtime_opts_v6.dns[0] : NULL, 0, IP_VER6, out_primary);
        if (out_secondary) make_ep(have_secondary ? v6->runtime_opts_v6.dns[1] : NULL, 0, IP_VER6, out_secondary);
        return have_primary || have_secondary;
    }

    uint8_t count = l2_interface_count();
    for (uint8_t i = 0; i < count; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2) continue;
        for (int s = 0; s < MAX_IPV4_PER_INTERFACE; s++){
            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
            if (!ipv4_l3_is_active(v4)) continue;

            uint32_t primary = v4->runtime_opts_v4.dns[0];
            uint32_t secondary = v4->runtime_opts_v4.dns[1];
            if (!primary && !secondary) continue;
            if (out_primary) make_ep(&primary, 0, IP_VER4, out_primary);
            if (out_secondary) make_ep(&secondary, 0, IP_VER4, out_secondary);
            return true;
        }

        for (int s = 0; s < MAX_IPV6_PER_INTERFACE; ++s) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[s];
            if (!ipv6_l3_is_ready(v6)) continue;
            bool have_primary = !ipv6_is_unspecified(v6->runtime_opts_v6.dns[0]);
            bool have_secondary = !ipv6_is_unspecified(v6->runtime_opts_v6.dns[1]);
            if (!have_primary && !have_secondary) continue;
            if (out_primary) make_ep(have_primary ? v6->runtime_opts_v6.dns[0] : NULL, 0, IP_VER6, out_primary);
            if (out_secondary) make_ep(have_secondary ? v6->runtime_opts_v6.dns[1] : NULL, 0, IP_VER6, out_secondary);
            return true;
        }
    }
    return false;
}

static dns_result_t dns_query_selected(l3_id_t l3_id, const net_l4_endpoint* primary, const net_l4_endpoint* secondary, dns_server_sel_t which, const char* hostname, dns_qtype_t qtype, uint32_t timeout_ms, dns_record_t* out_records, uint32_t max_records, uint32_t* out_count) {
    if (out_count) *out_count = 0;
    bool have_primary = primary && ((primary->ver == IP_VER4 && rd_be32(primary->ip)) || (primary->ver == IP_VER6 && !ipv6_is_unspecified(primary->ip)));
    bool have_secondary = secondary && ((secondary->ver == IP_VER4 && rd_be32(secondary->ip)) || (secondary->ver == IP_VER6 && !ipv6_is_unspecified(secondary->ip)));
    if (which == DNS_USE_PRIMARY && !have_primary) return DNS_ERR_NO_DNS;
    if (which == DNS_USE_SECONDARY && !have_secondary) return DNS_ERR_NO_DNS;
    if (which == DNS_USE_BOTH && !have_primary && !have_secondary) return DNS_ERR_NO_DNS;
    socket_handle_t sock = create_socket(PROTO_UDP, &(SocketOptions){.flags = SOCK_OPT_NONBLOCK});
    if (!sock) return DNS_ERR_SOCKET;
    if (l3_id) {
        SockBindSpec spec = {.kind = BIND_L3, .l3_id = l3_id};
        if (bind_socket(sock, &spec, 0) != SOCK_OK) {
            close_socket(sock);
            return DNS_ERR_SOCKET;
        }
    }

    dns_result_t res = DNS_ERR_NO_DNS;
    if (which == DNS_USE_PRIMARY)  res = perform_dns_query_once(sock, primary, hostname, qtype, timeout_ms, out_records, max_records, out_count);
    else if (which == DNS_USE_SECONDARY) res = perform_dns_query_once(sock, secondary, hostname, qtype, timeout_ms, out_records, max_records, out_count);
    else {
        const net_l4_endpoint* first = have_primary ? primary : secondary;
        const net_l4_endpoint* second = have_secondary ? secondary : primary;

        res = perform_dns_query_once(sock, first, hostname, qtype, timeout_ms, out_records, max_records, out_count);
        if (res != DNS_OK && second && first != second) res = perform_dns_query_once(sock, second, hostname, qtype, timeout_ms, out_records, max_records, out_count);
    }

    close_socket(sock);
    return res;
}

dns_result_t dns_query(const char* hostname, dns_qtype_t qtype, dns_record_t* out_records, uint32_t max_records, uint32_t* out_count, dns_server_sel_t which, uint32_t timeout_ms) {
    if (out_count) *out_count = 0;
    if (!hostname) return DNS_ERR_FORMAT;
    if (!out_records && max_records) return DNS_ERR_FORMAT;
    if (dns_wire_is_local_name(hostname)) return mdns_query(0, hostname, qtype, timeout_ms, out_records, max_records, out_count);

    net_l4_endpoint p, s;
    if (!pick_dns(0, &p, &s)) return DNS_ERR_NO_DNS;
    return dns_query_selected(0, &p, &s, which, hostname, qtype, timeout_ms, out_records, max_records, out_count);
}

dns_result_t dns_query_on_l3(l3_id_t l3_id, const char* hostname, dns_qtype_t qtype, dns_record_t* out_records, uint32_t max_records, uint32_t* out_count, dns_server_sel_t which, uint32_t timeout_ms) {
    if (out_count) *out_count = 0;
    if (!hostname) return DNS_ERR_FORMAT;
    if (!out_records && max_records) return DNS_ERR_FORMAT;
    if (!l3_id) return DNS_ERR_NO_DNS;
    if (dns_wire_is_local_name(hostname))return mdns_query(l3_id, hostname, qtype, timeout_ms, out_records, max_records, out_count);
    net_l4_endpoint p,s;
    if (!pick_dns(l3_id, &p, &s)) return DNS_ERR_NO_DNS;
    return dns_query_selected(l3_id, &p, &s, which, hostname, qtype, timeout_ms, out_records, max_records, out_count);
}

static dns_result_t dns_resolve_ip_common(uint8_t use_l3, l3_id_t l3_id, const char* hostname, dns_qtype_t qtype, uint8_t out_addr[16], dns_server_sel_t which, uint32_t timeout_ms, uint32_t *out_ttl_s) {
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

dns_result_t dns_resolve_a_on_l3(l3_id_t l3_id, const char* hostname, uint32_t* out_ip, dns_server_sel_t which, uint32_t timeout_ms) {
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

dns_result_t dns_resolve_aaaa_on_l3(l3_id_t l3_id, const char* hostname, uint8_t out_ipv6[16], dns_server_sel_t which, uint32_t timeout_ms) {
    if (!hostname || !out_ipv6) return DNS_ERR_FORMAT;
    if (dns_cache_get_ip(hostname, DNS_TYPE_AAAA, out_ipv6)) return DNS_OK;
    uint32_t ttl_s = 0;
    uint32_t mdns_timeout = timeout_ms > MDNS_TIMEOUT_AAAA_MS ? MDNS_TIMEOUT_AAAA_MS : timeout_ms;
    if (dns_wire_is_local_name(hostname)) return dns_resolve_ip_common(1, l3_id, hostname, DNS_TYPE_AAAA, out_ipv6, which, mdns_timeout, &ttl_s);
    return dns_resolve_ip_common(1, l3_id, hostname, DNS_TYPE_AAAA, out_ipv6, which, timeout_ms, &ttl_s);
}
