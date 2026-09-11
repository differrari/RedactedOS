#include "mdns_internal.h"
#include "dns_daemon.h"

#include "dns_sd.h"
#include "dns_cache.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/transport_layer/csocket.h"
#include "networking/interface_manager.h"
#include "data/hash.h"
#include "math/rng.h"
#include "random/random.h"
#include "std/std.h"
#include "syscalls/syscalls.h"

#define MDNS_ANNOUNCE_BURST 2
#define MDNS_ANNOUNCE_INTERVAL_MS 1000
#define MDNS_RECENT_RR_MAX 64
#define MDNS_PROBE_COUNT 3
#define MDNS_PROBE_INTERVAL_MS 250

typedef struct {
    bool used;
    uint8_t l2_slot;
    ip_version_t ver;
    uint64_t key;
    uint64_t last_ms;
} mdns_recent_rr_t;

char g_mdns_fqdn[72] = MDNS_HOST_NAME ".local";
uint16_t g_mdns_host_name_index;
uint16_t g_mdns_old_host_name_index;
bool g_mdns_host_advertised;
uint8_t g_mdns_host_goodbye_left;
mdns_link_state_t g_mdns_host_link[MAX_L2_INTERFACES];
mdns_service_t g_mdns_services[MDNS_MAX_SERVICES];

static rng_t g_mdns_rng;
static bool g_mdns_rng_ready;
static mdns_recent_rr_t g_mdns_recent_rr[MDNS_RECENT_RR_MAX];

bool mdns_send(socket_handle_t sock, const net_l4_endpoint *src, bool unicast, ip_version_t ver, const uint8_t *mcast_ip, const uint8_t *pkt, uint32_t pkt_len) {
    net_l4_endpoint dst;
    memset(&dst, 0, sizeof(dst));

    if (unicast && src) {
        dst = *src;
        if (!dst.port) dst.port = DNS_MDNS_PORT;
        return send_to_socket(sock, &dst, pkt, pkt_len) >= 0;
    }

    dst.ver = ver;
    if (ver == IP_VER4) memcpy(dst.ip, mcast_ip, 4);
    else memcpy(dst.ip, mcast_ip, 16);
    dst.port = DNS_MDNS_PORT;
    return send_to_socket(sock, &dst, pkt, pkt_len) >= 0;
}

static uint32_t mdns_probe_jitter(void) {
    if (!g_mdns_rng_ready) {
        rng_init_random(&g_mdns_rng);
        g_mdns_rng_ready = true;
    }
    return rng_between32(&g_mdns_rng, 0, MDNS_PROBE_INTERVAL_MS + 1);
}

void mdns_probe_start(mdns_probe_t* probe, uint32_t delay_ms) {
    probe->ready = false;
    probe->sent = 0;
    probe->next_ms = get_time() + delay_ms;
}

void mdns_instance_name(char* out, uint32_t out_cap, const mdns_service_t* s, uint16_t name_index) {
    char label[64];
    if (name_index < 2) strncpy(label, s->instance, sizeof(label));
    else {
        char suffix[12];
        string_format_buf(suffix, sizeof(suffix), "-%u", (uint32_t)name_index);
        uint32_t suffix_len = strlen(suffix);
        uint32_t base_len = strlen(s->instance);
        if (base_len + suffix_len > 63) base_len = 63 - suffix_len;
        memcpy(label, s->instance, base_len);
        memcpy(label + base_len, suffix, suffix_len + 1);
    }

    string_format_buf(out, out_cap, "%s._%s._%s.local", label, s->service, s->proto);
}

l2_interface_t* mdns_l2(l3_id_t l3_id) {
    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(l3_id);
    if (ipv4_l3_is_ready(v4)) return v4->l2;

    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(l3_id);
    return ipv6_l3_is_ready(v6) ? v6->l2 : NULL;
}

bool mdns_pkt_begin(mdns_pkt_t *p, uint8_t *out, uint32_t cap, uint16_t flags) {
    if (cap < 12) return false;

    memset(out, 0, 12);
    memset(p, 0, sizeof(*p));
    p->out = out;
    p->cap = cap;
    p->off = 12;
    wr_be16(out+2, flags);
    return true;
}

bool mdns_pkt_add(mdns_pkt_t *p, bool additional, const dns_record_t* record, uint16_t rrclass, uint32_t ttl_s) {
    uint32_t off = dns_sd_add_record(p->out, p->cap, p->off, record, rrclass, ttl_s);
    if (!off) return false;
    p->off = off;
    if (additional) {
        p->ar++;
        wr_be16(p->out + 10, p->ar);
    } else {
        p->an++;
        wr_be16(p->out + 6, p->an);
    }
    return true;
}

void mdns_cache_host(l2_interface_t* l2) {
    uint32_t ttl_ms = MDNS_TTL_S * 1000;
    for (uint8_t i = 0; i < MAX_IPV4_PER_INTERFACE; i++) {
        l3_ipv4_interface_t* v4 = l2->l3_v4[i];
        if (!ipv4_l3_is_ready(v4) || v4->is_localhost) continue;
        uint8_t ip[16] = {0};
        memcpy(ip, &v4->ip, 4);
        dns_cache_put_ip(g_mdns_fqdn, DNS_TYPE_A, ip, ttl_ms);
    }

    for (uint8_t i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (!ipv6_l3_is_ready(v6) || v6->is_localhost) continue;
        dns_cache_put_ip(g_mdns_fqdn, DNS_TYPE_AAAA, v6->ip, ttl_ms);
    }
}

static bool mdns_label_ok(const char *s, uint32_t max_len) {
    if (!s) return false;
    uint32_t len = strlen(s);
    if (!len || len >= max_len || len > 63) return false;
    for (uint32_t i = 0; i < len; i++) {
        char c = s[i];
        if (!is_alnum(c) && c != '-') return false;
    }
    return true;
}

static bool mdns_instance_ok(const char *s) {
    if (!s) return false;
    uint32_t len = strlen(s);
    if (!len || len >= 64 || len > 63) return false;
    for (uint32_t i = 0; i < len; i++) {
        char c = s[i];
        if (c < 32 || c == '.') return false;
    }
    return true;
}

void mdns_record(dns_record_t* r, const char* name, uint16_t type) {
    memset(r, 0, sizeof(*r));
    strncpy(r->name, name, sizeof(r->name));
    r->type = type;
    r->rrclass = DNS_CLASS_IN;
    r->ttl_s = MDNS_TTL_S;
}

uint32_t mdns_host_records(l2_interface_t* l2, dns_record_t* out, uint32_t cap) {
    if (!cap) return 0;

    uint32_t count = 0;
    for (uint8_t i = 0; i < MAX_IPV4_PER_INTERFACE && count < cap; i++) {
        l3_ipv4_interface_t* v4 = l2->l3_v4[i];
        if (!ipv4_l3_is_ready(v4) || v4->is_localhost) continue;
        mdns_record(&out[count], g_mdns_fqdn, DNS_TYPE_A);
        wr_be32(out[count].addr, v4->ip);
        count++;
    }

    for (uint8_t i = 0; i < MAX_IPV6_PER_INTERFACE && count < cap; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (!ipv6_l3_is_ready(v6) || v6->is_localhost) continue;
        mdns_record(&out[count], g_mdns_fqdn, DNS_TYPE_AAAA);
        memcpy(out[count].addr, v6->ip, 16);
        count++;
    }
    return count;
}

static int mdns_add_host_records(mdns_pkt_t* p, bool additional, l2_interface_t* l2, const char* name, uint16_t rrclass, uint32_t ttl_s) {
    if (!l2->is_up) return 0;

    dns_record_t records[8];
    uint32_t count = mdns_host_records(l2, records, N_ARR(records));
    for (uint32_t i = 0; i < count; i++) {
        strncpy(records[i].name, name, sizeof(records[i].name));
        if (!mdns_pkt_add(p, additional, &records[i], rrclass, ttl_s)) return -1;
    }
    return count;
}

void mdns_service_records(const mdns_service_t* s, uint16_t name_index, dns_record_t out[2]) {
    char inst[256];
    mdns_instance_name(inst, sizeof(inst), s, name_index);

    mdns_record(&out[0], inst, DNS_TYPE_TXT);
    strncpy(out[0].txt, s->txt, sizeof(out[0].txt));

    mdns_record(&out[1], inst, DNS_TYPE_SRV);
    out[1].port = s->port;
    strncpy(out[1].target, g_mdns_fqdn, sizeof(out[1].target));
}

static bool mdns_recent(const dns_record_t* r, ip_version_t ver, uint32_t l2_slot, uint64_t now, bool mark) {
    if (l2_slot >= MAX_L2_INTERFACES) return false;

    char name[DNS_WIRE_MAX_NAME];
    if (!dns_wire_name_normalize(r->name, name, sizeof(name))) return false;

    uint8_t key_data[DNS_WIRE_MAX_NAME*2 + 20];
    uint32_t off = 0;
    if (!dns_wire_write_name(key_data, sizeof(key_data), &off, name)) return false;
    off = dns_wire_put_u16(key_data, sizeof(key_data), off, r->type);
    if (!off) return false;
    off = dns_wire_put_u16(key_data, sizeof(key_data), off, r->rrclass & DNS_CLASS_MASK);
    if (!off) return false;
    off = dns_wire_put_u32(key_data, sizeof(key_data), off, r->ttl_s);
    if (!off) return false;

    uint8_t rdata[DNS_WIRE_MAX_NAME + 8];
    uint32_t rdata_len = dns_wire_write_record_rdata(r, rdata, sizeof(rdata));
    if (off + rdata_len > sizeof(key_data)) return false;
    memcpy(key_data + off, rdata, rdata_len);
    uint64_t key = hash_map_fnv1a64(key_data, off + rdata_len);

    int empty = -1;
    uint32_t oldest = 0;

    for (uint32_t i = 0; i < MDNS_RECENT_RR_MAX; i++) {
        mdns_recent_rr_t* e = &g_mdns_recent_rr[i];
        if (!e->used) {
            if (empty < 0) empty = (int)i;
            continue;
        }
        if (e->l2_slot == l2_slot && e->ver == ver && e->key == key) {
            bool recent = (now - e->last_ms) < MDNS_ANNOUNCE_INTERVAL_MS;
            if (mark) e->last_ms = now;
            return recent;
        }
        if (g_mdns_recent_rr[oldest].used && e->last_ms < g_mdns_recent_rr[oldest].last_ms) oldest = i;
    }

    if (mark) {
        mdns_recent_rr_t* e = &g_mdns_recent_rr[empty >= 0 ? (uint32_t)empty : oldest];
        e->used = true;
        e->l2_slot = (uint8_t)l2_slot;
        e->ver = ver;
        e->key = key;
        e->last_ms = now;
    }
    return false;
}

bool mdns_send_mcast(socket_handle_t sock, ip_version_t ver, const uint8_t* mcast_ip, uint32_t l2_slot, const uint8_t* pkt, uint32_t pkt_len, uint64_t now) {
    if (l2_slot >= MAX_L2_INTERFACES) return mdns_send(sock, NULL, false, ver, mcast_ip, pkt, pkt_len);

    dns_record_t records[48];
    uint32_t count = 0;
    uint16_t flags = 0;
    if (!dns_wire_parse_records(pkt, pkt_len, false, 0, records, N_ARR(records), &count, &flags)) return false;

    uint8_t filtered[1500];
    mdns_pkt_t p;
    uint64_t added = 0;
    if (!mdns_pkt_begin(&p, filtered, sizeof(filtered), flags)) return false;
    for (uint32_t i = 0; i < count; i++) {
        if (mdns_recent(&records[i], ver, l2_slot, now, false)) continue;
        bool additional = records[i].section == DNS_SECTION_ADDITIONAL;
        if (!mdns_pkt_add(&p, additional, &records[i], records[i].rrclass, records[i].ttl_s)) return false;
        added |= 1ULL << i;
    }
    if (!added || !mdns_send(sock, NULL, false, ver, mcast_ip, filtered, p.off)) return false;
    for (uint32_t i = 0; i < count; i++) if (added & (1ULL << i)) mdns_recent(&records[i], ver, l2_slot, now, true);
    return true;
}

static bool mdns_probe_packet(uint8_t* out, uint32_t cap, const char* name, const dns_record_t* records, uint32_t count, uint32_t* out_len) {
    if (cap < 12 || !count) return false;
    memset(out, 0, 12);
    wr_be16(out + 4, 1);
    wr_be16(out + 8, (uint16_t)count);

    uint32_t off = 12;
    if (!dns_wire_write_name(out, cap, &off, name)) return false;
    off = dns_wire_put_u16(out, cap, off, DNS_TYPE_ANY);
    if (!off) return false;
    off = dns_wire_put_u16(out, cap, off, DNS_CLASS_IN);
    if (!off) return false;

    for (uint32_t i = 0; i < count; i++) {
        off = dns_sd_add_record(out, cap, off, &records[i], DNS_CLASS_IN, MDNS_TTL_S);
        if (!off) return false;
    }

    *out_len = off;
    return true;
}

bool mdns_has_work(void) {
    if (g_mdns_host_goodbye_left) return true;
    for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) if (g_mdns_services[i].used && (g_mdns_services[i].active || g_mdns_services[i].goodbye_left)) return true;
    return false;
}

bool mdns_register_service(const char *instance, const char *service, const char *proto, uint16_t port, const char *txt) {
    if (!port) return false;
    if (!mdns_instance_ok(instance)) return false;
    if (!mdns_label_ok(service, 32)) return false;
    if (!proto) return false;
    if (strcmp_case(proto, "tcp", true) != 0 && strcmp_case(proto, "udp", true) != 0) return false;
    if (txt) {
        uint32_t txt_len = strlen(txt);
        if (txt_len >= 128) return false;
        for (uint32_t i = 0; i < txt_len; i++) {
            char c = txt[i];
            if (!is_printable(c)) return false;
        }
    }

    uint32_t link_mask = 0;
    uint8_t n_if = l2_interface_count();
    for (uint8_t i = 0; i < n_if; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2 || !l2->is_up || !l2->ifindex || l2->ifindex > MAX_L2_INTERFACES) continue;

        bool ready = false;
        for (uint8_t j = 0; j < MAX_IPV4_PER_INTERFACE && !ready; j++) {
            l3_ipv4_interface_t* v4 = l2->l3_v4[j];
            if (ipv4_l3_is_ready(v4) && !v4->is_localhost) ready = true;
        }
        for (uint8_t j = 0; j < MAX_IPV6_PER_INTERFACE && !ready; j++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[j];
            if (ipv6_l3_is_ready(v6) && !v6->is_localhost) ready = true;
        }
        if (ready) link_mask |= 1u << (l2->ifindex-1);
    }

    for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
        mdns_service_t *s = &g_mdns_services[i];
        if (!s->used) continue;
        if (strncmp(s->instance, instance, (int)sizeof(s->instance)) != 0) continue;
        if (strncmp(s->service, service, (int)sizeof(s->service)) != 0) continue;
        if (strncmp_case(s->proto, proto, true, (int)sizeof(s->proto)) != 0) continue;

        bool was_active = s->active;
        bool rdata_changed = s->port != port;
        if (txt) rdata_changed |= strcmp(s->txt, txt) != 0;
        else rdata_changed |= s->txt[0] != 0;

        s->active = true;
        s->port = port;
        if (txt) strncpy(s->txt, txt, sizeof(s->txt));
        else s->txt[0] = 0;

        bool cancelled_goodbye = false;
        if (!was_active && s->goodbye_left) {
            s->goodbye_left = 0;
            s->retire = false;
            cancelled_goodbye = true;
        }

        for (uint32_t slot = 0; slot < MAX_L2_INTERFACES; slot++) {
            if (!(link_mask & (1u << slot))) continue;
            mdns_link_state_t* link = &s->link[slot];
            if (!cancelled_goodbye && !was_active && !s->advertised) mdns_probe_start(&link->probe, mdns_probe_jitter());
            else if (was_active && rdata_changed && !link->probe.ready) mdns_probe_start(&link->probe, mdns_probe_jitter());
            link->announce_left = link->probe.ready && g_mdns_host_link[slot].probe.ready ? MDNS_ANNOUNCE_BURST : 0;
            link->last_tx_ms = 0;

            mdns_probe_t* host_probe = &g_mdns_host_link[slot].probe;
            if (!host_probe->ready && !host_probe->sent && !host_probe->next_ms) mdns_probe_start(host_probe, mdns_probe_jitter());
        }

        s->goodbye_left = 0;
        s->last_tx_ms = 0;
        g_mdns_host_goodbye_left = 0;
        dns_daemon_kick();
        return true;
    }

    for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
        mdns_service_t *s = &g_mdns_services[i];
        if (s->used) continue;

        memset(s, 0, sizeof(*s));
        s->used = true;
        s->active = true;
        s->port = port;

        strncpy(s->instance, instance, sizeof(s->instance));
        strncpy(s->service, service, sizeof(s->service));
        strncpy(s->proto, proto, sizeof(s->proto));
        if (txt) strncpy(s->txt, txt, sizeof(s->txt));

        for (uint32_t slot = 0; slot < MAX_L2_INTERFACES; slot++) {
            if (!(link_mask & (1u << slot))) continue;
            mdns_probe_start(&s->link[slot].probe, mdns_probe_jitter());

            mdns_probe_t* host_probe = &g_mdns_host_link[slot].probe;
            if (!host_probe->ready && !host_probe->sent && !host_probe->next_ms) mdns_probe_start(host_probe, mdns_probe_jitter());
        }
        s->goodbye_left = 0;
        s->last_tx_ms = 0;
        g_mdns_host_goodbye_left = 0;
        dns_daemon_kick();
        return true;
    }

    return false;
}

bool mdns_deregister_service(const char *instance, const char *service, const char *proto) {
    if (!mdns_instance_ok(instance)) return false;
    if (!mdns_label_ok(service, 32)) return false;
    if (!proto) return false;
    if (strcmp_case(proto, "tcp", true) != 0 && strcmp_case(proto, "udp", true) != 0) return false;

    for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
        mdns_service_t *s = &g_mdns_services[i];
        if (!s->used) continue;
        if (!s->active) continue;
        if (strncmp(s->instance, instance, (int)sizeof(s->instance)) != 0) continue;
        if (strncmp(s->service, service, (int)sizeof(s->service)) != 0) continue;
        if (strncmp_case(s->proto, proto, true, (int)sizeof(s->proto)) != 0) continue;

        s->active = false;
        s->last_tx_ms = 0;
        for (uint32_t slot = 0; slot < MAX_L2_INTERFACES; slot++) {
            s->link[slot].announce_left = 0;
            s->link[slot].last_tx_ms = 0;
        }

        if (s->advertised) {
            s->old_name_index = s->name_index;
            s->goodbye_left = MDNS_GOODBYE_BURST;
            s->retire = true;
        } else memset(s, 0, sizeof(*s));

        bool active = false;
        for (uint32_t j = 0; j < MDNS_MAX_SERVICES; j++) {
            if (!g_mdns_services[j].used || !g_mdns_services[j].active) continue;
            active = true;
            break;
        }
        if (!active) {
            if (g_mdns_host_advertised) {
                g_mdns_old_host_name_index = g_mdns_host_name_index;
                g_mdns_host_goodbye_left = MDNS_GOODBYE_BURST;
            }
            memset(g_mdns_host_link, 0, sizeof(g_mdns_host_link));
        }
        dns_daemon_kick();
        return true;
    }

    return false;
}

void mdns_reprobe(uint32_t l2_mask) {
    for (uint32_t i = 0; i < MDNS_RECENT_RR_MAX; i++) {
        mdns_recent_rr_t *e = &g_mdns_recent_rr[i];
        if (e->used && e->l2_slot < MAX_L2_INTERFACES && (l2_mask & (1u << e->l2_slot))) memset(e, 0, sizeof(*e));
    }

    uint32_t delay_ms = mdns_probe_jitter();
    for (uint32_t slot = 0; slot < MAX_L2_INTERFACES; slot++) {
        if (!(l2_mask & (1u << slot))) continue;

        bool active = false;
        g_mdns_host_link[slot].announce_left = 0;
        g_mdns_host_link[slot].last_tx_ms = 0;
        for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
            mdns_service_t* s = &g_mdns_services[i];
            if (!s->used || !s->active) continue;
            active = true;
            mdns_probe_start(&s->link[slot].probe, delay_ms);
            s->link[slot].announce_left = 0;
            s->link[slot].last_tx_ms = 0;
        }
        if (active) mdns_probe_start(&g_mdns_host_link[slot].probe, delay_ms);
        else memset(&g_mdns_host_link[slot], 0, sizeof(g_mdns_host_link[slot]));
    }
}

void mdns_responder_tick_multi(const mdns_tx_target_t *targets, uint32_t target_count) {
    if (!target_count) return;

    uint64_t now = get_time();
    for (uint32_t slot = 0; slot < MAX_L2_INTERFACES; slot++) {
        mdns_link_state_t* host = &g_mdns_host_link[slot];
        if (!host->probe.next_ms || host->probe.ready || now < host->probe.next_ms) continue;

        if (host->probe.sent < MDNS_PROBE_COUNT) {
            bool sent = false;
            for (uint32_t t = 0; t < target_count; t++) {
                if (targets[t].ifindex != slot + 1) continue;
                l2_interface_t* l2 = mdns_l2(targets[t].l3_id);
                if (!l2) continue;

                dns_record_t records[8];
                uint32_t count = mdns_host_records(l2, records, N_ARR(records));
                if (!count) continue;

                uint8_t pkt[900];
                uint32_t pkt_len = 0;
                if (!mdns_probe_packet(pkt, sizeof(pkt), g_mdns_fqdn, records, count, &pkt_len)) continue;
                if (mdns_send(targets[t].sock, NULL, false, targets[t].ver, targets[t].mcast_ip, pkt, pkt_len)) sent = true;
            }
            if (sent) {
                host->probe.sent++;
                host->probe.next_ms = now + MDNS_PROBE_INTERVAL_MS;
            }
        } else {
            host->probe.ready = true;
            host->announce_left = MDNS_ANNOUNCE_BURST;
            host->last_tx_ms = 0;
            for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
                mdns_service_t *s = &g_mdns_services[i];
                if (s->used && s->active && s->link[slot].probe.ready) {
                    s->link[slot].announce_left = MDNS_ANNOUNCE_BURST;
                    s->link[slot].last_tx_ms = 0;
                }
            }
        }
    }

    for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
        mdns_service_t *s = &g_mdns_services[i];
        if (!s->used || !s->active) continue;

        for (uint32_t slot = 0; slot < MAX_L2_INTERFACES; slot++) {
            mdns_link_state_t* link = &s->link[slot];
            if (link->probe.ready || !link->probe.next_ms || now < link->probe.next_ms) continue;

            if (link->probe.sent < MDNS_PROBE_COUNT) {
                dns_record_t records[2];
                mdns_service_records(s, s->name_index, records);
                bool sent = false;

                uint8_t pkt[900];
                uint32_t pkt_len = 0;
                if (mdns_probe_packet(pkt, sizeof(pkt), records[0].name, records, 2, &pkt_len)) {
                    for (uint32_t t = 0; t < target_count; t++) {
                        if (targets[t].ifindex != slot + 1) continue;
                        if (mdns_send(targets[t].sock, NULL, false, targets[t].ver, targets[t].mcast_ip, pkt, pkt_len)) sent = true;
                    }
                }
                if (sent) {
                    link->probe.sent++;
                    link->probe.next_ms = now + MDNS_PROBE_INTERVAL_MS;
                }
            } else {
                link->probe.ready = true;
                if (g_mdns_host_link[slot].probe.ready) {
                    link->announce_left = MDNS_ANNOUNCE_BURST;
                    link->last_tx_ms = 0;
                }
            }
        }
    }

    if (g_mdns_host_goodbye_left) {
        char old_name[72];
        if (g_mdns_old_host_name_index < 2) string_format_buf(old_name, sizeof(old_name), "%s.local", MDNS_HOST_NAME);
        else string_format_buf(old_name, sizeof(old_name), "%s-%u.local", MDNS_HOST_NAME, (uint32_t)g_mdns_old_host_name_index);
        bool sent = false;
        for (uint32_t t = 0; t < target_count; t++) {
            l2_interface_t* l2 = mdns_l2(targets[t].l3_id);
            if (!l2) continue;

            uint8_t pkt[900];
            mdns_pkt_t p;
            if (!mdns_pkt_begin(&p, pkt, sizeof(pkt), DNS_FLAG_QR | DNS_FLAG_AA)) continue;
            int added = mdns_add_host_records(&p, false, l2, old_name, MDNS_FLUSH_CLASS, 0);
            if (added <= 0) continue;
            uint32_t slot = l2->ifindex && l2->ifindex <= MAX_L2_INTERFACES ? (uint32_t)l2->ifindex - 1u : UINT32_MAX;
            if (mdns_send_mcast(targets[t].sock, targets[t].ver, targets[t].mcast_ip, slot, pkt, p.off, now)) sent = true;
        }

        if (sent) {
            g_mdns_host_goodbye_left--;
            if (!g_mdns_host_goodbye_left) {
                g_mdns_host_advertised = false;
                memset(g_mdns_host_link, 0, sizeof(g_mdns_host_link));
            }
        }
    }

    for (uint32_t slot = 0; slot < MAX_L2_INTERFACES; slot++) {
        mdns_link_state_t* host = &g_mdns_host_link[slot];
        if (!host->probe.ready || !host->announce_left) continue;
        if (host->last_tx_ms && (now - host->last_tx_ms) < MDNS_ANNOUNCE_INTERVAL_MS) continue;

        bool sent = false;
        for (uint32_t t = 0; t < target_count; t++) {
            if (targets[t].ifindex != slot + 1) continue;
            l2_interface_t* l2 = mdns_l2(targets[t].l3_id);
            if (!l2) continue;

            uint8_t pkt[900];
            mdns_pkt_t p;
            if (!mdns_pkt_begin(&p, pkt, sizeof(pkt), DNS_FLAG_QR | DNS_FLAG_AA)) continue;
            int added = mdns_add_host_records(&p, false, l2, g_mdns_fqdn, MDNS_FLUSH_CLASS, MDNS_TTL_S);
            if (added <= 0) continue;
            if (mdns_send_mcast(targets[t].sock, targets[t].ver, targets[t].mcast_ip, slot, pkt, p.off, now)) {
                sent = true;
                mdns_cache_host(l2);
            }
        }
        if (sent) {
            g_mdns_host_advertised = true;
            host->last_tx_ms = now;
            host->announce_left--;
        }
    }

    for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
        mdns_service_t *s = &g_mdns_services[i];
        if (!s->used) continue;
        if (s->goodbye_left && s->last_tx_ms && (now - s->last_tx_ms) < MDNS_ANNOUNCE_INTERVAL_MS) continue;

        bool other_type = false;
        if (s->goodbye_left && s->retire) {
            for (uint32_t j = 0; j < MDNS_MAX_SERVICES; j++) {
                if (j == i) continue;
                const mdns_service_t* other = &g_mdns_services[j];
                if (!other->used || !other->active) continue;
                if (strncmp(other->service, s->service, (int)sizeof(other->service)) != 0) continue;
                if (strncmp_case(other->proto, s->proto, true, (int)sizeof(other->proto)) != 0) continue;
                other_type = true;
                break;
            }
        }
        bool add_enum = !s->goodbye_left || (s->retire && !other_type);
        uint16_t name_index = s->goodbye_left ? s->old_name_index : s->name_index;
        for (uint32_t slot = 0; slot < MAX_L2_INTERFACES; slot++) {
            mdns_link_state_t* link = NULL;
            if (s->goodbye_left) {
                if (slot) break;
            } else {
                if (!s->active) break;
                link = &s->link[slot];
                if (!link->probe.ready || !g_mdns_host_link[slot].probe.ready || !link->announce_left) continue;
                if (link->last_tx_ms && (now - link->last_tx_ms) < MDNS_ANNOUNCE_INTERVAL_MS) continue;
            }

            bool sent = false;
            for (uint32_t t = 0; t < target_count; t++) {
                if (!s->goodbye_left && targets[t].ifindex != slot + 1) continue;
                l2_interface_t* l2 = mdns_l2(targets[t].l3_id);
                if (!l2) continue;
                uint32_t l2_slot = l2->ifindex && l2->ifindex <= MAX_L2_INTERFACES ? (uint32_t)l2->ifindex - 1u : UINT32_MAX;

                uint8_t pkt[900];
                mdns_pkt_t p;
                if (!mdns_pkt_begin(&p, pkt, sizeof(pkt), DNS_FLAG_QR | DNS_FLAG_AA)) continue;

                char type[128];
                string_format_buf(type, sizeof(type), "_%s._%s.local", s->service, s->proto);
                dns_record_t unique[2];
                mdns_service_records(s, name_index, unique);
                if (!unique[0].name[0] || !s->service[0] || !s->proto[0]) continue;

                uint32_t ttl = s->goodbye_left ? 0 : MDNS_TTL_S;
                dns_record_t record;
                if (add_enum) {
                    mdns_record(&record, DNS_SD_ENUM_SERVICES, DNS_TYPE_PTR);
                    strncpy(record.target, type, sizeof(record.target));
                    if (!mdns_pkt_add(&p, false, &record, DNS_CLASS_IN, ttl)) continue;
                }

                mdns_record(&record, type, DNS_TYPE_PTR);
                strncpy(record.target, unique[0].name, sizeof(record.target));
                if (!mdns_pkt_add(&p, false, &record, DNS_CLASS_IN, ttl)) continue;
                if (!mdns_pkt_add(&p, true, &unique[1], MDNS_FLUSH_CLASS, ttl)) continue;
                if (!mdns_pkt_add(&p, true, &unique[0], MDNS_FLUSH_CLASS, ttl)) continue;
                if (!s->goodbye_left && mdns_add_host_records(&p, true, l2, g_mdns_fqdn, MDNS_FLUSH_CLASS, MDNS_TTL_S) < 0) continue;
                if (mdns_send_mcast(targets[t].sock, targets[t].ver, targets[t].mcast_ip, l2_slot, pkt, p.off, now)) sent = true;
            }
            if (!sent) continue;

            if (s->goodbye_left) {
                s->last_tx_ms = now;
                s->goodbye_left--;
                if (!s->goodbye_left) {
                    if (s->retire) memset(s, 0, sizeof(*s));
                    else s->last_tx_ms = 0;
                }
                break;
            } else {
                s->advertised = true;
                link->last_tx_ms = now;
                link->announce_left--;
            }
        }
    }
}
