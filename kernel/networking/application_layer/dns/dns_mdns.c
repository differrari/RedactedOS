#include "dns_mdns.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/interface_manager.h"
#include "std/std.h"
#include "networking/transport_layer/trans_utils.h"

#define MDNS_QUERY_TARGETS_MAX (MAX_L2_INTERFACES * 2)

typedef struct {
    socket_handle_t sock;
    l3_id_t l3_id;
    net_l4_endpoint dst;
} mdns_query_target_t;
//TODO keep query id for legacy unicast mdns replies
//TODO do the same per L3 handling for igmp and mld
//161 192.168.1.100		37669	224.0.0.251		5353	False	RedactedOS._http._tcp.local RedactedOS._http._tcp.local: type SRV, class IN, "QU" question Transaction ID: 0x34f0
//164 5353	192.168.1.100		37669	True	 Transaction ID: 0x0000 RedactedOS._http._tcp.local: type SRV, class IN, cache flush, priority 0, weight 0, port 80, target RedactedOS.local 1... .... .... .... = Cache flush: True
static bool mdns_open_query_target(l3_id_t l3_id, mdns_query_target_t* target) {
    if (!l3_id || !target) return false;

    ip_version_t ver;
    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(l3_id);
    if (ipv4_l3_is_ready(v4) && !v4->is_localhost) ver = IP_VER4;
    else {
        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(l3_id);
        if (!ipv6_l3_is_ready(v6) || v6->is_localhost) return false;
        ver = IP_VER6;
    }

    socket_handle_t sock = create_socket(PROTO_UDP, &(SocketOptions){.flags = SOCK_OPT_TTL | SOCK_OPT_NONBLOCK, .ttl = 255});
    if (!sock) return false;

    SockBindSpec spec = {.kind = BIND_L3, .ver = ver, .l3_id = l3_id};
    if (bind_socket(sock, &spec, 0) != SOCK_OK) {
        close_socket(sock);
        return false;
    }

    target->sock = sock;
    target->l3_id = l3_id;
    if (ver == IP_VER4) {
        uint32_t group = DNS_MDNS_GROUP_V4;
        make_ep(&group, DNS_MDNS_PORT, IP_VER4, &target->dst);
    } else {
        uint8_t group6[16];
        ipv6_make_multicast(0x02, IPV6_MCAST_MDNS, 0, group6);
        make_ep(group6, DNS_MDNS_PORT, IP_VER6, &target->dst);
    }
    return true;
}

dns_result_t mdns_query(l3_id_t l3_id, const char* name, dns_qtype_t qtype, uint32_t timeout_ms, dns_record_t* out_records, uint32_t max_records, uint32_t* out_count) {
    if (out_count) *out_count = 0;
    if (!name) return DNS_ERR_FORMAT;
    if (!out_records && max_records) return DNS_ERR_FORMAT;

    uint8_t request_buffer[512];
    uint32_t request_len = dns_wire_build_query(request_buffer, sizeof(request_buffer), 0, name, qtype, false);
    if (!request_len) return DNS_ERR_FORMAT;

    mdns_query_target_t targets[MDNS_QUERY_TARGETS_MAX];
    uint32_t target_count = 0;
    if (l3_id) {
        if (mdns_open_query_target(l3_id, &targets[0])) target_count = 1;
    } else {
        uint8_t n_if = l2_interface_count();
        for (uint8_t i = 0; i < n_if && target_count < MDNS_QUERY_TARGETS_MAX; i++) {
            l2_interface_t* l2 = l2_interface_at(i);
            if (!l2 || !l2->is_up) continue;

            for (uint8_t j = 0; j < MAX_IPV4_PER_INTERFACE; j++) {
                l3_ipv4_interface_t* v4 = l2->l3_v4[j];
                if (!ipv4_l3_is_ready(v4) || v4->is_localhost) continue;
                if (mdns_open_query_target(v4->l3_id, &targets[target_count])) target_count++;
                break;
            }

            if (target_count >= MDNS_QUERY_TARGETS_MAX) break;
            for (uint8_t j = 0; j < MAX_IPV6_PER_INTERFACE; j++) {
                l3_ipv6_interface_t* v6 = l2->l3_v6[j];
                if (!ipv6_l3_is_ready(v6) || v6->is_localhost) continue;
                if (mdns_open_query_target(v6->l3_id, &targets[target_count])) target_count++;
                break;
            }
        }
    }
    if (!target_count) return DNS_ERR_NO_DNS;

    bool sent_any = false;
    for (uint32_t i = 0; i < target_count; i++) if (send_to_socket(targets[i].sock, &targets[i].dst, request_buffer, request_len) >= 0) sent_any = true;
    if (!sent_any) {
        for (uint32_t i = 0; i < target_count; i++) close_socket(targets[i].sock);
        return DNS_ERR_SEND;
    }

    uint32_t found = 0;
    bool matched = false;
    uint32_t waited_ms = 0;
    while (waited_ms < timeout_ms) {
        for (uint32_t t = 0; t < target_count; t++) {
            for (uint32_t rx = 0; rx < 16; rx++) {
                uint8_t response_buffer[1024];
                net_l4_endpoint source;
                int64_t received = receive_from_socket(targets[t].sock, response_buffer, sizeof(response_buffer), &source);
                if (received == SOCK_ERR_WOULDBLOCK) break;
                if (received <= 0) break;
                if (source.port != DNS_MDNS_PORT) continue;

                bool source_on_l3 = false;
                if (source.ver == IP_VER4) {
                    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(targets[t].l3_id);
                    if (ipv4_l3_is_ready(v4) && v4->mask) {
                        uint32_t ip = 0;
                        memcpy(&ip, source.ip, 4);
                        source_on_l3 = ipv4_same_subnet(v4->ip, ip, v4->mask);
                    }
                } else if (source.ver == IP_VER6) {
                    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(targets[t].l3_id);
                    if (ipv6_l3_is_ready(v6)) source_on_l3 = ipv6_is_linklocal(source.ip) || (v6->prefix_len && ipv6_common_prefix_len(v6->ip, source.ip) >= v6->prefix_len);
                }

                if (!source_on_l3) continue;

                uint32_t received_len = received;
                dns_record_t records[12];
                uint32_t count = 0;
                uint16_t flags = 0;
                if (dns_wire_parse_records(response_buffer, received_len, false, 0, records, N_ARR(records), &count, &flags) && (flags & DNS_FLAG_QR) && !(flags & (DNS_OPCODE_MASK | DNS_RCODE_MASK))) {
                    for (uint32_t i = 0; i < count; i++) {
                        if ((records[i].rrclass & DNS_CLASS_MASK) != DNS_CLASS_IN) continue;
                        if (qtype != DNS_TYPE_ANY && records[i].type != qtype) continue;
                        if (!dns_wire_name_equals(records[i].name, name)) continue;
                        matched = true;
                        if (max_records && found < max_records) {
                            bool duplicate = false;
                            for (uint32_t j = 0; j < found; j++) {
                                if (!dns_wire_record_equal(&records[i], &out_records[j])) continue;
                                duplicate = true;
                                break;
                            }
                            if (!duplicate) out_records[found++] = records[i];
                        }
                    }
                }
            }
        }

        if (max_records && found >= max_records) break;
        msleep(20);
        waited_ms += 20;
    }

    for (uint32_t i = 0; i < target_count; i++) close_socket(targets[i].sock);
    if (out_count) *out_count = found;
    return matched ? DNS_OK : DNS_ERR_TIMEOUT;
}

dns_result_t mdns_resolve_a(const char* name, uint32_t timeout_ms, uint32_t* out_ip, uint32_t* out_ttl_s) {
    if (!out_ip) return DNS_ERR_FORMAT;

    dns_record_t records[4];
    uint32_t count = 0;

    dns_result_t r = mdns_query(0, name, DNS_TYPE_A, timeout_ms, records, 4, &count);
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
    dns_result_t r = mdns_query(0, name, DNS_TYPE_AAAA, timeout_ms, records, 4, &count);
    if (r != DNS_OK) return r;

    for (uint32_t i = 0; i < count; i++) {
        if (records[i].type != DNS_TYPE_AAAA) continue;
        memcpy(out_ipv6, records[i].addr, 16);
        if (out_ttl_s) *out_ttl_s = records[i].ttl_s;
        return DNS_OK;
    }

    return DNS_ERR_NO_ANSWER;
}
