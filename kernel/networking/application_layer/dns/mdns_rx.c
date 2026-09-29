#include "mdns_internal.h"

#include "dns_cache.h"
#include "dns_sd.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "data/hash.h"
#include "std/std.h"
#include "syscalls/syscalls.h"

#define MDNS_QUERY_DEDUP_MAX 8
#define MDNS_QUERY_DEDUP_MS 250
#define MDNS_PROBE_TIE_RETRY_MS 1000

typedef struct {
    bool used;
    uint8_t l2_slot;
    ip_version_t ver;
    uint16_t port;
    uint64_t hash;
    uint64_t last_ms;
    uint8_t ip[16];
} mdns_query_dedup_t;

static mdns_query_dedup_t g_mdns_query_dedup[MDNS_QUERY_DEDUP_MAX];
static uint8_t g_mdns_query_dedup_next = 0;

static int mdns_probe_compare(const dns_record_t* own, uint32_t own_count, const dns_record_t* records, uint32_t record_count, const char* name) {
    dns_record_t lhs[8];
    dns_record_t rhs[8];
    uint32_t lc = 0;
    uint32_t rc = 0;

    for (uint32_t i = 0; i < own_count && lc < N_ARR(lhs); i++) lhs[lc++] = own[i];
    for (uint32_t i = 0; i < record_count && rc < N_ARR(rhs); i++) {
        if (records[i].section != DNS_SECTION_AUTHORITY) continue;
        if ((records[i].rrclass & DNS_CLASS_MASK) != DNS_CLASS_IN) continue;
        if (!dns_wire_name_equals(records[i].name, name)) continue;
        rhs[rc++] = records[i];
    }
    if (!rc) return 0;

    for (uint32_t i = 1; i < lc; i++) {
        dns_record_t cur = lhs[i];
        uint32_t j = i;
        for (; j && dns_wire_record_cmp(&lhs[j - 1], &cur) > 0; j--) lhs[j] = lhs[j - 1];
        lhs[j] = cur;
    }
    for (uint32_t i = 1; i < rc; i++) {
        dns_record_t cur = rhs[i];
        uint32_t j = i;
        for (; j && dns_wire_record_cmp(&rhs[j - 1], &cur) > 0; j--) rhs[j] = rhs[j - 1];
        rhs[j] = cur;
    }
    for (uint32_t i = 0; i < lc && i < rc; i++) {
        int cmp = dns_wire_record_cmp(&lhs[i], &rhs[i]);
        if (cmp) return cmp;
    }
    if (lc == rc) return 0;
    return lc > rc ? 1 : -1;
}

static int mdns_add_answer(mdns_pkt_t* p, bool additional, const dns_record_t* record, const dns_record_t* known, uint32_t known_count, uint16_t rrclass, uint32_t ttl_s) {
    for (uint32_t i = 0; i < known_count; i++) {
        if (known[i].section != DNS_SECTION_ANSWER) continue;
        if (known[i].ttl_s < record->ttl_s / 2) continue;
        if (dns_wire_record_equal(&known[i], record)) return 0;
    }
    return mdns_pkt_add(p, additional, record, rrclass, ttl_s) ? 1 : -1;
}

static void mdns_host_conflict(void) {
    if (g_mdns_host_advertised && !g_mdns_host_goodbye_left) {
        g_mdns_old_host_name_index = g_mdns_host_name_index;
        g_mdns_host_goodbye_left = MDNS_GOODBYE_BURST;
    }

    g_mdns_host_advertised = false;
    if (g_mdns_host_name_index < 2 || g_mdns_host_name_index == UINT16_MAX) g_mdns_host_name_index = 2; 
    else g_mdns_host_name_index++;
    string_format_buf(g_mdns_fqdn, sizeof(g_mdns_fqdn), "%s-%u.local", MDNS_HOST_NAME, (uint32_t)g_mdns_host_name_index);

    for (uint32_t slot = 0; slot < MAX_L2_INTERFACES; slot++) {
        mdns_probe_start(&g_mdns_host_link[slot].probe, MDNS_PROBE_RETRY_MS);
        g_mdns_host_link[slot].announce_left = 0;
        g_mdns_host_link[slot].last_tx_ms = 0;

        for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
            mdns_service_t* s = &g_mdns_services[i];
            if (!s->used || !s->active) continue;
            if (!s->link[slot].probe.ready) mdns_probe_start(&s->link[slot].probe, MDNS_PROBE_RETRY_MS);
            s->link[slot].announce_left = 0;
            s->link[slot].last_tx_ms = 0;
        }
    }
}

static void mdns_service_conflict(mdns_service_t* s) {
    if (s->advertised && !s->goodbye_left) {
        s->old_name_index = s->name_index;
        s->goodbye_left = MDNS_GOODBYE_BURST;
        s->retire = false;
    }

    s->advertised = false;
    if (s->name_index < 2 || s->name_index == UINT16_MAX) s->name_index = 2;
    else s->name_index++;
    for (uint32_t slot = 0; slot < MAX_L2_INTERFACES; slot++) {
        mdns_probe_start(&s->link[slot].probe, MDNS_PROBE_RETRY_MS);
        s->link[slot].announce_left = 0;
        s->link[slot].last_tx_ms = 0;
    }
}

static bool mdns_has_conflict(const dns_record_t* records, uint32_t count, const dns_record_t* own, uint32_t own_count) {
    for (uint32_t i = 0; i < count; i++) {
        if ((records[i].rrclass & DNS_CLASS_MASK) != DNS_CLASS_IN) continue;
        bool own_type = false;
        bool same = false;
        for (uint32_t j = 0; j < own_count; j++) {
            if (records[i].type != own[j].type) continue;
            if (!dns_wire_name_equals(records[i].name, own[j].name)) continue;
            own_type = true;
            if (dns_wire_record_equal(&records[i], &own[j])) {
                same = true;
                break;
            }
        }
        if (own_type && !same) return true;
    }
    return false;
}

static void mdns_rx_response(l2_interface_t* l2, dns_record_t* records, uint32_t count) {
    if (!l2->ifindex || l2->ifindex > MAX_L2_INTERFACES) return;
    uint32_t l2_slot = (uint32_t)l2->ifindex - 1;

    bool host_name = false;
    for (uint32_t i = 0; i < count; i++) {
        dns_record_t* r = &records[i];
        if ((r->rrclass & DNS_CLASS_MASK) != DNS_CLASS_IN) continue;
        if (dns_wire_name_equals(r->name, g_mdns_fqdn)) host_name = true;
        if (r->type == DNS_TYPE_A) {
            if (!r->ttl_s) dns_cache_remove_ip(r->name, DNS_TYPE_A);
            else dns_cache_put_ip(r->name, DNS_TYPE_A, r->addr, r->ttl_s * 1000);
        } else if (r->type == DNS_TYPE_AAAA) {
            if (!r->ttl_s) dns_cache_remove_ip(r->name, DNS_TYPE_AAAA);
            else dns_cache_put_ip(r->name, DNS_TYPE_AAAA, r->addr, r->ttl_s * 1000);
        }
    }

    if (g_mdns_host_link[l2_slot].probe.sent && !g_mdns_host_link[l2_slot].probe.ready && host_name) {
        mdns_host_conflict();
        return;
    }

    if (g_mdns_host_link[l2_slot].probe.ready) {
        dns_record_t own[8];
        uint32_t own_count = mdns_host_records(l2, own, N_ARR(own));
        if (own_count && mdns_has_conflict(records, count, own, own_count)) {
            mdns_host_conflict();
            return;
        }
    }

    for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
        mdns_service_t* s = &g_mdns_services[i];
        if (!s->used || !s->active) continue;

        char inst[256];
        mdns_instance_name(inst, sizeof(inst), s, s->name_index);
        if (s->link[l2_slot].probe.sent && !s->link[l2_slot].probe.ready) {
            bool seen = false;
            for (uint32_t j = 0; j < count; j++) {
                if ((records[j].rrclass & DNS_CLASS_MASK) != DNS_CLASS_IN) continue;
                if (!dns_wire_name_equals(records[j].name, inst)) continue;
                seen = true;
                break;
            }
            if (seen) {
                mdns_service_conflict(s);
                continue;
            }
        }
        if (!s->link[l2_slot].probe.ready) continue;

        dns_record_t own[2];
        mdns_service_records(s, s->name_index, own);
        if (mdns_has_conflict(records, count, own, 2)) mdns_service_conflict(s);
    }
}

static void mdns_rx_probe(l2_interface_t* l2, const uint8_t* pkt, uint32_t pkt_len, uint16_t qd, const dns_record_t* records, uint32_t count) {
    if (!l2->ifindex || l2->ifindex > MAX_L2_INTERFACES) return;
    uint32_t l2_slot = (uint32_t)l2->ifindex - 1;

    uint32_t off = 12;
    for (uint16_t qi = 0; qi < qd; qi++) {
        char qname[256];
        uint32_t next = 0;
        if (!dns_wire_read_name(pkt, pkt_len, off, qname, sizeof(qname), &next)) break;
        if (next + 4 > pkt_len) break;

        uint16_t qtype = rd_be16(pkt + next);
        uint16_t qclass = rd_be16(pkt + next + 2) & DNS_CLASS_MASK;
        off = next + 4;
        if (qtype != DNS_TYPE_ANY || (qclass != DNS_CLASS_IN && qclass != DNS_CLASS_ANY)) continue;

        if (!g_mdns_host_link[l2_slot].probe.ready && g_mdns_host_link[l2_slot].probe.sent && dns_wire_name_equals(qname, g_mdns_fqdn)) {
            dns_record_t own[8];
            uint32_t own_count = mdns_host_records(l2, own, N_ARR(own));
            if (own_count && mdns_probe_compare(own, own_count, records, count, g_mdns_fqdn) < 0) {
                mdns_probe_start(&g_mdns_host_link[l2_slot].probe, MDNS_PROBE_TIE_RETRY_MS);
                g_mdns_host_link[l2_slot].announce_left = 0;
                g_mdns_host_link[l2_slot].last_tx_ms = 0;
            }
        }

        for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
            mdns_service_t* s = &g_mdns_services[i];
            if (!s->used || !s->active || s->link[l2_slot].probe.ready || !s->link[l2_slot].probe.sent) continue;

            char inst[256];
            mdns_instance_name(inst, sizeof(inst), s, s->name_index);
            if (!dns_wire_name_equals(qname, inst)) continue;

            dns_record_t own[2];
            mdns_service_records(s, s->name_index, own);
            if (mdns_probe_compare(own, 2, records, count, inst) < 0) {
                mdns_probe_start(&s->link[l2_slot].probe, MDNS_PROBE_TIE_RETRY_MS);
                s->link[l2_slot].announce_left = 0;
                s->link[l2_slot].last_tx_ms = 0;
            }
        }
    }
}

void mdns_rx(socket_handle_t sock, l3_id_t l3_id, ip_version_t ver, const uint8_t *mcast_ip, const uint8_t *pkt, uint32_t pkt_len, const net_l4_endpoint *src) {
    if (pkt_len < 12) return;

    l2_interface_t* l2 = mdns_l2(l3_id);
    if (!l2) return;
    if (!l2->ifindex || l2->ifindex > MAX_L2_INTERFACES) return;
    uint32_t l2_slot = (uint32_t)l2->ifindex - 1;

    dns_record_t records[48];
    uint32_t record_count = 0;
    uint16_t flags = 0;
    if (!dns_wire_parse_records(pkt, pkt_len, false, 0, records, N_ARR(records), &record_count, &flags)) return;
    if (flags & (DNS_OPCODE_MASK | DNS_RCODE_MASK)) return;

    if (flags & DNS_FLAG_QR) {
        if (src->port != DNS_MDNS_PORT) return;
        mdns_rx_response(l2, records, record_count);
        return;
    }

    uint16_t qd = rd_be16(pkt + 4);
    if (!qd) return;

    mdns_rx_probe(l2, pkt, pkt_len, qd, records, record_count);

    uint64_t now = get_time();
    uint64_t hash = hash_map_fnv1a64(pkt, pkt_len);
    for (uint32_t i = 0; i < MDNS_QUERY_DEDUP_MAX; i++) {
        mdns_query_dedup_t *e = &g_mdns_query_dedup[i];
        if (!e->used || e->l2_slot != l2_slot || e->ver != ver || e->port != src->port || e->hash != hash) continue;
        if (memcmp(e->ip, src->ip, ver == IP_VER4 ? 4 : 16) != 0) continue;
        if (now - e->last_ms >= MDNS_QUERY_DEDUP_MS) continue;
        e->last_ms = now;
        return;
    }

    mdns_query_dedup_t *dedup = &g_mdns_query_dedup[g_mdns_query_dedup_next++ % MDNS_QUERY_DEDUP_MAX];
    memset(dedup, 0, sizeof(*dedup));
    dedup->used = true;
    dedup->l2_slot = (uint8_t)l2_slot;
    dedup->ver = ver;
    dedup->port = src->port;
    dedup->hash = hash;
    dedup->last_ms = now;
    memcpy(dedup->ip, src->ip, ver == IP_VER4 ? 4 : 16);

    bool legacy = src->port && src->port != DNS_MDNS_PORT;
    bool want_unicast = false;
    bool need_mcast = false;
    uint32_t ttl = legacy ? 10 : MDNS_TTL_S;
    uint16_t unique_class = legacy ? DNS_CLASS_IN : MDNS_FLUSH_CLASS;
    const dns_record_t* known = legacy ? NULL : records;
    uint32_t known_count = legacy ? 0 : record_count;

    uint8_t out[1500];
    mdns_pkt_t p;
    uint16_t reply_flags = DNS_FLAG_QR | DNS_FLAG_AA;
    if (legacy) reply_flags |= rd_be16(pkt + 2) & DNS_FLAG_RD;
    if (!mdns_pkt_begin(&p, out, sizeof(out), reply_flags)) return;
    if (legacy) {
        uint32_t qend = 12;
        for (uint16_t i = 0; i < qd; i++) {
            if (!dns_wire_read_name(pkt, pkt_len, qend, NULL, 0, &qend)) return;
            if (qend + 4 > pkt_len) return;
            qend += 4;
        }
        if (qend > sizeof(out)) return;
        memcpy(out + 12, pkt + 12, qend - 12);
        p.off = qend;
        wr_be16(out, rd_be16(pkt));
        wr_be16(out + 4, qd);
    }

    dns_record_t host_records[8];
    uint32_t host_count = g_mdns_host_link[l2_slot].probe.ready ? mdns_host_records(l2, host_records, N_ARR(host_records)) : 0;
    bool has_host = false;
    uint32_t service_bits = 0;
    uint32_t qoff = 12;

    for (uint16_t qi = 0; qi < qd; qi++) {
        char qname[256];
        uint32_t next = 0;
        if (!dns_wire_read_name(pkt, pkt_len, qoff, qname, sizeof(qname), &next)) return;
        if (next + 4 > pkt_len) return;

        uint16_t qtype = rd_be16(pkt + next);
        uint16_t qclass = rd_be16(pkt + next + 2);
        qoff = next + 4;
        if ((qclass & DNS_CLASS_MASK) != DNS_CLASS_IN && (qclass & DNS_CLASS_MASK) != DNS_CLASS_ANY) continue;
        bool qu = (qclass & DNS_CLASS_UNICAST_RESPONSE) != 0;
        uint16_t records_before = p.an + p.ar;

        if (g_mdns_host_link[l2_slot].probe.ready && (qtype == DNS_TYPE_PTR || qtype == DNS_TYPE_ANY)) {
            char norm[DNS_WIRE_MAX_NAME];
            uint32_t ipq = 0;
            bool valid_reverse = dns_wire_name_normalize(qname, norm, sizeof(norm));
            if (valid_reverse) {
                uint8_t ip[4];
                const char *p = norm;
                for (int i = 3; i >= 0; i--) {
                    const char *label = p;
                    uint32_t v = 0;
                    while (is_digit(*p) && (p - label) < 3) v = v * 10 + (*p++ - '0');//strtoul might be more readable TODO
                    if (p == label || is_digit(*p) || v > 255) {
                        valid_reverse = false;
                        break;
                    }
                    ip[i] = (uint8_t)v;
                    if (i && *p++ != '.') {
                        valid_reverse = false;
                        break;
                    }
                }
                if (valid_reverse) {
                    if (*p++ != '.' || strcmp(p, "in-addr.arpa") != 0) valid_reverse = false;
                    else ipq = rd_be32(ip);
                }
            }

            if (valid_reverse) {
                for (uint8_t i = 0; i < MAX_IPV4_PER_INTERFACE; i++) {
                    l3_ipv4_interface_t* v4 = l2->l3_v4[i];
                    if (!ipv4_l3_is_ready(v4) || v4->is_localhost || v4->ip != ipq) continue;

                    dns_record_t ptr;
                    mdns_record(&ptr, qname, DNS_TYPE_PTR);
                    strncpy(ptr.target, g_mdns_fqdn, sizeof(ptr.target));
                    int ans = mdns_add_answer(&p, false, &ptr, known, known_count, DNS_CLASS_IN, ttl);
                    if (ans < 0) return;
                    if (ans) {
                        has_host = true;
                        for (uint32_t h = 0; h < host_count; h++) {
                            int added = mdns_add_answer(&p, true, &host_records[h], known, known_count, unique_class, ttl);
                            if (added < 0) return;
                            has_host |= added != 0;
                        }
                    }
                    break;
                }
            }
        }

        if (g_mdns_host_link[l2_slot].probe.ready && dns_wire_name_equals(qname, g_mdns_fqdn)) {
            for (uint32_t h = 0; h < host_count; h++) {
                if (qtype != DNS_TYPE_ANY && qtype != host_records[h].type) continue;
                int added = mdns_add_answer(&p, false, &host_records[h], known, known_count, unique_class, ttl);
                if (added < 0) return;
                has_host |= added != 0;
            }
        }

        if ((qtype == DNS_TYPE_PTR || qtype == DNS_TYPE_ANY) && dns_wire_name_equals(qname, DNS_SD_ENUM_SERVICES)) {
            for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
                mdns_service_t* s = &g_mdns_services[i];
                if (!(s->used && s->active && s->link[l2_slot].probe.ready && g_mdns_host_link[l2_slot].probe.ready)) continue;

                char type[128];
                string_format_buf(type, sizeof(type), "_%s._%s.local", s->service, s->proto);
                bool seen = false;
                for (uint32_t j = 0; j < i; j++) {
                    if (!(g_mdns_services[j].used && g_mdns_services[j].active && g_mdns_services[j].link[l2_slot].probe.ready && g_mdns_host_link[l2_slot].probe.ready)) continue;
                    char type2[128];
                    string_format_buf(type2, sizeof(type2), "_%s._%s.local", g_mdns_services[j].service, g_mdns_services[j].proto);
                    if (dns_wire_name_equals(type2, type)) {
                        seen = true;
                        break;
                    }
                }
                if (seen) continue;

                dns_record_t ptr;
                mdns_record(&ptr, DNS_SD_ENUM_SERVICES, DNS_TYPE_PTR);
                strncpy(ptr.target, type, sizeof(ptr.target));
                int added = mdns_add_answer(&p, false, &ptr, known, known_count, DNS_CLASS_IN, ttl);
                if (added < 0) return;
                if (added) service_bits |= 1u << i;
            }
        }

        bool need_host_add = false;
        for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
            mdns_service_t* s = &g_mdns_services[i];
            if (!(s->used && s->active && s->link[l2_slot].probe.ready && g_mdns_host_link[l2_slot].probe.ready)) continue;

            char type[128];
            string_format_buf(type, sizeof(type), "_%s._%s.local", s->service, s->proto);
            dns_record_t unique[2];

            mdns_service_records(s, s->name_index, unique);

            if ((qtype == DNS_TYPE_PTR || qtype == DNS_TYPE_ANY) && dns_wire_name_equals(qname, type)) {
                dns_record_t ptr;
                mdns_record(&ptr, type, DNS_TYPE_PTR);
                strncpy(ptr.target, unique[0].name, sizeof(ptr.target));
                int added = mdns_add_answer(&p, false, &ptr, known, known_count, DNS_CLASS_IN, ttl);
                if (added < 0) return;
                if (added) {
                    added = mdns_add_answer(&p, true, &unique[1], known, known_count, unique_class, ttl);
                    if (added < 0) return;
                    if (added) {
                        service_bits |= 1u << i;
                        need_host_add = true;
                    }
                    added = mdns_add_answer(&p, true, &unique[0], known, known_count, unique_class, ttl);
                    if (added < 0) return;
                    if (added) service_bits |= 1u << i;
                }
            }

            if ((qtype == DNS_TYPE_SRV || qtype == DNS_TYPE_ANY) && dns_wire_name_equals(qname, unique[0].name)) {
                int added = mdns_add_answer(&p, false, &unique[1], known, known_count, unique_class, ttl);
                if (added < 0) return;
                if (added) {
                    service_bits |= 1u << i;
                    need_host_add = true;
                }
            }

            if ((qtype == DNS_TYPE_TXT || qtype == DNS_TYPE_ANY) && dns_wire_name_equals(qname, unique[0].name)) {
                int added = mdns_add_answer(&p, false, &unique[0], known, known_count, unique_class, ttl);
                if (added < 0) return;
                if (added) service_bits |= 1u << i;
            }
        }

        if (need_host_add) {
            for (uint32_t h = 0; h < host_count; h++) {
                int added = mdns_add_answer(&p, true, &host_records[h], known, known_count, unique_class, ttl);
                if (added < 0) return;
                has_host |= added != 0;
            }
        }

        if (!legacy && p.an + p.ar != records_before) {
            if (qu) want_unicast = true;
            else need_mcast = true;
        }
    }

    if (!p.an && !p.ar) return;
    bool sent;
    if (legacy || (want_unicast && !need_mcast)) sent = mdns_send(sock, src, true, ver, mcast_ip, out, p.off);
    else sent = mdns_send_mcast(sock, ver, mcast_ip, l2_slot, out, p.off, now);
    if (!sent) return;

    if (has_host) {
        g_mdns_host_advertised = true;
        mdns_cache_host(l2);
    }
    for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) if ((service_bits & (1u << i)) && g_mdns_services[i].used) g_mdns_services[i].advertised = true;
}
