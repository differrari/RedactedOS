#include "dns_mdns.h"
#include "dns_daemon.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "std/std.h"
#include "networking/transport_layer/trans_utils.h"

static dns_result_t perform_mdns_query_once(socket_handle_t sock, const net_l4_endpoint *dst, const char *name, dns_qtype_t qtype, uint32_t timeout_ms, dns_record_t *out_records, uint32_t max_records, uint32_t *out_count) {
    if (out_count) *out_count = 0;
    if (!sock) return DNS_ERR_NO_DNS;
    if (!dst) return DNS_ERR_NO_DNS;
    if (!name) return DNS_ERR_FORMAT;
    if (!out_records && max_records) return DNS_ERR_FORMAT;

    uint8_t request_buffer[512];
    uint32_t offset = dns_wire_build_query(request_buffer, sizeof(request_buffer), 0, name, qtype, false);
    if (!offset) return DNS_ERR_FORMAT;

    int64_t sent = socket_sendto_udp(sock, DST_ENDPOINT, dst, 0, request_buffer, offset);
    if (sent < 0) return DNS_ERR_SEND;

    uint32_t found = 0;
    uint32_t waited_ms = 0;
    while (waited_ms < timeout_ms) {
        uint8_t response_buffer[512];
        net_l4_endpoint source;
        int64_t received = socket_recvfrom_udp(sock, response_buffer, sizeof(response_buffer), &source);
        if (received > 0 && source.port == DNS_MDNS_PORT){
            uint32_t received_len = received;
            dns_record_t records[12];
            uint32_t count = 0;
            uint16_t flags = 0;
            if (dns_wire_parse_records(response_buffer, received_len, false, 0, records,12, &count, &flags) && (flags & DNS_FLAG_QR)) {
                for (uint32_t i = 0; i < count; i++) {
                    if ((records[i].rrclass & DNS_CLASS_MASK) != DNS_CLASS_IN) continue;
                    if (qtype != DNS_TYPE_ANY && records[i].type != qtype) continue;
                    if (!dns_wire_name_equals(records[i].name, name)) continue;
                    if (found < max_records) out_records[found] = records[i];
                    found++;
                    if (found >= max_records && max_records) {
                        if (out_count) *out_count = max_records;
                        return DNS_OK;
                    }
                }
            }
        }

        msleep(20);
        waited_ms += 20;
    }

    if (found) {
        if (out_count) *out_count = found < max_records ? found : max_records;
        return DNS_OK;
    }

    return DNS_ERR_TIMEOUT;
}

dns_result_t mdns_query(const char* name, dns_qtype_t qtype, uint32_t timeout_ms, dns_record_t* out_records, uint32_t max_records, uint32_t* out_count) {
    if (out_count) *out_count = 0;
    if (!name) return DNS_ERR_FORMAT;
    if (!out_records && max_records) return DNS_ERR_FORMAT;

    dns_result_t last = DNS_ERR_NO_DNS;
    uint32_t total = 0;

    socket_handle_t sock4 = mdns_socket_handle_v4();
    if (sock4) {
        uint32_t group =DNS_MDNS_GROUP_V4;
        net_l4_endpoint dst;
        make_ep(group, DNS_MDNS_PORT, IP_VER4, &dst);
        uint32_t got = 0;
        last = perform_mdns_query_once(sock4, &dst, name, qtype, timeout_ms, out_records, max_records, &got);
        if (last == DNS_OK) total = got;
        if (total >= max_records && max_records) {
            if (out_count) *out_count  =total;
            return DNS_OK;
        }
    }

    socket_handle_t sock6 = mdns_socket_handle_v6();
    if (sock6) {
        net_l4_endpoint dst;
        memset(&dst, 0, sizeof(dst));
        dst.ver = IP_VER6;
        ipv6_make_multicast(0x02, IPV6_MCAST_MDNS, 0, dst.ip);
        dst.port = DNS_MDNS_PORT;
        uint32_t got = 0;
        dns_record_t *dst_records = out_records ? out_records + total : 0;
        uint32_t left = max_records > total ? max_records - total : 0;
        dns_result_t r6 = perform_mdns_query_once(sock6, &dst, name, qtype, timeout_ms, dst_records, left, &got);
        if (r6 == DNS_OK) {
            total += got;
            last = DNS_OK;
        } else if (last != DNS_OK) last = r6;
    }

    if (total) {
        if (out_count) *out_count = total;
        return DNS_OK;
    }

    return last;
}

dns_result_t mdns_resolve_a(const char* name, uint32_t timeout_ms, uint32_t* out_ip, uint32_t* out_ttl_s) {
    if (!out_ip) return DNS_ERR_FORMAT;

    dns_record_t records[4];
    uint32_t count = 0;

    dns_result_t r = mdns_query(name, DNS_TYPE_A, timeout_ms, records, 4, &count);
    if (r != DNS_OK) return r;

    for (uint32_t i = 0; i < count; i++) {
        if (records[i].type != DNS_TYPE_A) continue;
        *out_ip = rd_be32(records[i].addr);
        if (out_ttl_s) *out_ttl_s = records[i].ttl_s;
        return DNS_OK;
    }

    return DNS_ERR_NO_ANSWER;
}

dns_result_t mdns_resolve_aaaa(const char* name, uint32_t timeout_ms, uint8_t out_ipv6[16], uint32_t* out_ttl_s) {
    if (!out_ipv6) return DNS_ERR_FORMAT;
    dns_record_t records[4];
    uint32_t count = 0;
    dns_result_t r = mdns_query(name, DNS_TYPE_AAAA, timeout_ms, records, 4, &count);
    if (r != DNS_OK) return r;

    for (uint32_t i = 0; i < count; i++) {
        if (records[i].type != DNS_TYPE_AAAA) continue;
        memcpy(out_ipv6, records[i].addr, 16);
        if (out_ttl_s) *out_ttl_s = records[i].ttl_s;
        return DNS_OK;
    }

    return DNS_ERR_NO_ANSWER;
}
