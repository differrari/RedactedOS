#include "mdns_responder.h"

#include "dns_sd.h"
#include "dns_cache.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/transport_layer/csocket.h"
#include "networking/interface_manager.h"
#include "data/hash.h"
#include "std/std.h"
#include "std/string.h"
#include "syscalls/syscalls.h"

#define MDNS_TTL_S 120
#define MDNS_ANNOUNCE_BURST 3
#define MDNS_GOODBYE_BURST 3
#define MDNS_ANNOUNCE_INTERVAL_MS 250
#define MDNS_KEEPALIVE_MS 60000
#define MDNS_MAX_SERVICES 8
#define MDNS_CACHE_MAX 48
#define MDNS_QUERY_DEDUP_MAX 8
#define MDNS_QUERY_DEDUP_MS 250
#define MDNS_FLUSH_CLASS (DNS_CLASS_CACHE_FLUSH | DNS_CLASS_IN)
#define MDNS_HOST_NAME "RedactedOS"

typedef struct {
    bool used;
    bool active;
    uint8_t announce_left;
    uint8_t goodbye_left;
    uint64_t last_tx_ms;
    char instance[64];
    char service[32];
    char proto[8];
    char txt[128];
    uint16_t port;
} mdns_service_t;

typedef struct {
    uint8_t type;
    uint16_t rrtype;
    uint16_t port;
    uint64_t expire_ms;
    char name[256];
    char target[256];
    char txt[256];
} mdns_cache_entry_t;

typedef struct {
    bool used;
    ip_version_t ver;
    uint16_t port;
    uint64_t hash;
    uint64_t last_ms;
    uint8_t ip[16];
} mdns_query_dedup_t;

typedef struct {
    uint8_t *out;
    uint32_t cap;
    uint32_t off;
    uint32_t an_pos;
    uint32_t ar_pos;
    uint16_t an;
    uint16_t ar;
} mdns_pkt_t;

static uint32_t g_mdns_ipv4 = 0;
static uint8_t g_mdns_ipv6[16];
static uint8_t g_mdns_ifindex = 0;
static char g_mdns_fqdn[72];

static uint64_t g_mdns_last_refresh_ms = 0;
static uint64_t g_mdns_last_keepalive_ms = 0;
static uint8_t g_mdns_host_announce_left = 0;
static uint64_t g_mdns_host_last_tx_ms = 0;

static mdns_service_t g_mdns_services[MDNS_MAX_SERVICES];
static mdns_cache_entry_t g_mdns_cache[MDNS_CACHE_MAX];
static mdns_query_dedup_t g_mdns_query_dedup[MDNS_QUERY_DEDUP_MAX];
static uint8_t g_mdns_query_dedup_next = 0;

static void mdns_send(socket_handle_t sock, const net_l4_endpoint *src, bool unicast, ip_version_t ver, const uint8_t *mcast_ip, const uint8_t *pkt, uint32_t pkt_len) {
    if (!sock) return;
    if (!pkt) return;
    if (!pkt_len) return;

    net_l4_endpoint dst;
    memset(&dst, 0, sizeof(dst));

    if (unicast && src) {
        dst = *src;
        if (!dst.port) dst.port = DNS_MDNS_PORT;
        send_to_socket(sock, &dst, pkt, pkt_len);
        return;
    }

    dst.ver = ver;
    if (ver == IP_VER4) memcpy(dst.ip, mcast_ip, 4);
    else memcpy(dst.ip, mcast_ip, 16);
    dst.port = DNS_MDNS_PORT;
    send_to_socket(sock, &dst, pkt, pkt_len);
}

static bool mdns_pick_identity(uint32_t *out_v4, uint8_t out_v6[16], uint8_t *out_ifindex) {
    if (!out_v4) return false;
    if (!out_v6) return false;
    if (!out_ifindex) return false;

    uint32_t v4 = 0;
    uint8_t v6_best[16];
    uint8_t v6_fallback[16];
    uint8_t if_best = 0;
    uint8_t if_fallback = 0;

    memset(v6_best, 0, sizeof(v6_best));
    memset(v6_fallback, 0, sizeof(v6_fallback));

    uint8_t c = l2_interface_count();
    for (uint8_t i = 0; i < c; i++) {
        l2_interface_t *l2 = l2_interface_at(i);
        if (!l2 || !l2->is_up) continue;

        if (!v4) {
            for (uint8_t j = 0; j < l2->ipv4_count; j++) {
                l3_ipv4_interface_t *a = l2->l3_v4[j];
                if (!ipv4_l3_is_ready(a) || a->is_localhost) continue;
                v4 = a->ip;
                break;
            }
        }

        for (uint8_t j = 0; j < l2->ipv6_count; j++) {
            l3_ipv6_interface_t *a = l2->l3_v6[j];
            if (!ipv6_l3_is_ready(a) || a->is_localhost) continue;

            if (!ipv6_is_linklocal(a->ip) && !if_best) {
                memcpy(v6_best, a->ip, 16);
                if_best = l2->ifindex;
            }

            if (!if_fallback) {
                memcpy(v6_fallback, a->ip, 16);
                if_fallback = l2->ifindex;
            }
        }
    }

    if (if_best) {
        *out_v4 = v4;
        memcpy(out_v6, v6_best, 16);
        *out_ifindex = if_best;
        return true;
    }

    if (if_fallback) {
        *out_v4 = v4;
        memcpy(out_v6, v6_fallback, 16);
        *out_ifindex = if_fallback;
        return true;
    }

    if (v4) {
        *out_v4 = v4;
        memset(out_v6, 0, 16);
        *out_ifindex = 0;
        return true;
    }

    return false;
}

static void mdns_refresh_identity(void) {
    uint64_t now = get_time();
    if (g_mdns_last_refresh_ms && (now - g_mdns_last_refresh_ms) < 1000) return;
    g_mdns_last_refresh_ms = now;

    uint32_t v4 = 0;
    uint8_t v6[16];
    uint8_t ifindex = 0;
    memset(v6, 0, sizeof(v6));

    if (!mdns_pick_identity(&v4, v6, &ifindex)) return;

    bool changed = false;
    if (g_mdns_ipv4 != v4) changed = true;
    if (memcmp(g_mdns_ipv6, v6, 16) != 0) changed = true;
    if (g_mdns_ifindex != ifindex) changed = true;

    g_mdns_ipv4 = v4;
    memcpy(g_mdns_ipv6, v6, 16);
    g_mdns_ifindex = ifindex;

    if (!g_mdns_fqdn[0]) {
        string_format_buf(g_mdns_fqdn, sizeof(g_mdns_fqdn), "%s.local", MDNS_HOST_NAME);
        changed = true;
    }

    if (changed) {
        g_mdns_host_announce_left = MDNS_ANNOUNCE_BURST;
        g_mdns_host_last_tx_ms= 0;
        for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
            if (!g_mdns_services[i].used) continue;
            if (!g_mdns_services[i].active) continue;
            g_mdns_services[i].announce_left = MDNS_ANNOUNCE_BURST;
            g_mdns_services[i].last_tx_ms = 0;
        }
    }
}


static void mdns_make_service_type(char *out, uint32_t out_cap, const char *service, const char *proto) {
    if (!out) return;
    if (!out_cap) return;
    if (!service) return;
    if (!proto) return;
    string_format_buf(out, out_cap, "_%s._%s.local", service, proto);
}


static bool mdns_pkt_begin(mdns_pkt_t *p, uint8_t *out, uint32_t cap, uint16_t flags) {
    if (!p) return false;
    if (!out) return false;
    if (cap < 12) return false;

    memset(p, 0, sizeof(*p));
    p->out = out;
    p->cap = cap;
    p->off = 0;

    p->off = dns_wire_put_u16(out, cap, p->off, 0);
    if (!p->off) return false;
    p->off = dns_wire_put_u16(out, cap, p->off, flags);
    if (!p->off) return false;
    p->off = dns_wire_put_u16(out, cap, p->off, 0);
    if (!p->off) return false;

    p->an_pos = p->off;
    p->off = dns_wire_put_u16(out, cap, p->off, 0);
    if (!p->off) return false;

    p->off = dns_wire_put_u16(out, cap, p->off, 0);
    if (!p->off) return false;

    p->ar_pos = p->off;
    p->off = dns_wire_put_u16(out, cap, p->off, 0);
    if (!p->off) return false;

    return true;
}

static void mdns_pkt_commit(mdns_pkt_t *p) {
    if (!p) return;
    uint16_t anbe = be16(p->an);
    uint16_t arbe = be16(p->ar);
    memcpy(p->out + p->an_pos, &anbe, 2);
    memcpy(p->out + p->ar_pos, &arbe, 2);
}

static bool mdns_pkt_add_ptr(mdns_pkt_t *p, bool additional, const char *name, uint16_t rrclass, uint32_t ttl_s, const char *target) {
    if (!p) return false;
    uint32_t n = dns_sd_add_rr_ptr(p->out, p->cap, p->off, name, rrclass, ttl_s, target);
    if (!n) return false;
    p->off = n;
    if (additional) p->ar++;
    else p->an++;
    return true;
}

static bool mdns_pkt_add_a(mdns_pkt_t *p, bool additional, const char *name, uint16_t rrclass, uint32_t ttl_s, uint32_t ip) {
    if (!p) return false;
    uint32_t n = dns_sd_add_rr_a(p->out, p->cap, p->off, name, rrclass, ttl_s, ip);
    if (!n) return false;
    p->off = n;
    if (additional) p->ar++;
    else p->an++;
    return true;
}

static bool mdns_pkt_add_aaaa(mdns_pkt_t *p, bool additional, const char *name, uint16_t rrclass, uint32_t ttl_s, const uint8_t ip6[16]) {
    if (!p) return false;
    uint32_t n = dns_sd_add_rr_aaaa(p->out, p->cap, p->off, name, rrclass, ttl_s, ip6);
    if (!n) return false;
    p->off = n;
    if (additional) p->ar++;
    else p->an++;
    return true;
}

static bool mdns_pkt_add_srv(mdns_pkt_t *p, bool additional, const char *name, uint16_t rrclass, uint32_t ttl_s, uint16_t port, const char *target) {
    if (!p) return false;
    uint32_t n = dns_sd_add_rr_srv(p->out, p->cap, p->off, name, rrclass, ttl_s, 0, 0, port, target);
    if (!n) return false;
    p->off = n;
    if (additional) p->ar++;
    else p->an++;
    return true;
}

static bool mdns_pkt_add_txt(mdns_pkt_t *p, bool additional, const char *name, uint16_t rrclass, uint32_t ttl_s, const char *txt) {
    if (!p) return false;
    uint32_t n = dns_sd_add_rr_txt(p->out, p->cap, p->off, name, rrclass, ttl_s, txt);
    if (!n) return false;
    p->off = n;
    if (additional) p->ar++;
    else p->an++;
    return true;
}


static mdns_cache_entry_t *mdns_cache_entry_for(const char *name, uint16_t rrtype, bool create) {
    if (!name) return NULL;

    mdns_cache_entry_t *empty = NULL;
    for (uint32_t i = 0; i < MDNS_CACHE_MAX; i++) {
        mdns_cache_entry_t *e = &g_mdns_cache[i];
        if (!e->type) {
            if (!empty) empty = e;
            continue;
        }
        if (e->rrtype == rrtype && dns_wire_name_equals(e->name, name)) return e;
    }

    if (!create || !empty) return NULL;
    memset(empty, 0, sizeof(*empty));
    empty->type = 1;
    empty->rrtype = rrtype;
    strncpy(empty->name, name, sizeof(empty->name));
    return empty;
}

static bool mdns_current_ipv6(uint8_t ip[16]) {
    if (!ip) return false;

    memcpy(ip, g_mdns_ipv6, 16);
    if (ipv6_is_unspecified(ip) && g_mdns_ifindex) ipv6_make_lla_from_mac(g_mdns_ifindex, ip);
    return !ipv6_is_unspecified(ip);
}

static bool mdns_parse_ipv4_ptr_qname(const char *name, uint32_t *out_ip) {
    if (!name) return false;
    if (!out_ip) return false;

    char norm[DNS_WIRE_MAX_NAME];
    if (!dns_wire_name_normalize(name, norm, sizeof(norm))) return false;

    uint8_t ip[4];
    const char *p = norm;
    for (int i = 3; i >= 0; i--) {
        const char *label = p;
        uint32_t v = 0;
        while (is_digit(*p) && (p - label) < 3) v = v * 10 + (*p++ - '0');
        if (p == label) return false;
        if (is_digit(*p)) return false;
        if (v > 255) return false;
        ip[i] = v;
        if (i) {
            if (*p != '.') return false;
            p++;
        }
    }

    if (*p++ != '.') return false;

    if (strcmp(p, "in-addr.arpa") != 0) return false;
    *out_ip = rd_be32(ip);
    return true;
}

static void mdns_cache_from_packet(const uint8_t *pkt, uint32_t pkt_len) {
    if (!pkt) return;
    if (pkt_len < 12) return;

    dns_record_t records[12];
    uint32_t count = 0;
    uint16_t flags = 0;
    if (!dns_wire_parse_records(pkt, pkt_len, false, 0, records, 12, &count, &flags)) return;
    if (!(flags & DNS_FLAG_QR)) return;

    for (uint32_t i = 0; i < count; i++) {
        dns_record_t *r = &records[i];
        if ((r->rrclass & DNS_CLASS_MASK) != DNS_CLASS_IN) continue;

        switch (r->type) {
        case DNS_TYPE_A:
            if (!r->ttl_s) dns_cache_remove_ip(r->name, DNS_TYPE_A);
            else dns_cache_put_ip(r->name, DNS_TYPE_A, r->addr, r->ttl_s * 1000);
            break;
        case DNS_TYPE_AAAA:
            if (!r->ttl_s) dns_cache_remove_ip(r->name, DNS_TYPE_AAAA);
            else dns_cache_put_ip(r->name, DNS_TYPE_AAAA, r->addr, r->ttl_s * 1000);
            break;
        case DNS_TYPE_PTR:
        case DNS_TYPE_SRV:
        case DNS_TYPE_TXT:
            if (r->ttl_s && (r->type == DNS_TYPE_PTR || r->type == DNS_TYPE_SRV) && !r->target[0]) continue;

            mdns_cache_entry_t *e = mdns_cache_entry_for(r->name, r->type, r->ttl_s != 0);
            if (!e) continue;
            if (!r->ttl_s) {
                memset(e, 0, sizeof(*e));
                continue;
            }

            if (r->type == DNS_TYPE_SRV) e->port = r->port;
            if (r->type == DNS_TYPE_PTR || r->type == DNS_TYPE_SRV) strncpy(e->target, r->target, sizeof(e->target));
            if (r->type == DNS_TYPE_TXT) strncpy(e->txt, r->txt, sizeof(e->txt));
            e->expire_ms = get_time() + (uint64_t)r->ttl_s * 1000;
            break;
        default:
            break;
        }
    }
}

static bool mdns_add_host_additionals(mdns_pkt_t *p) {
    if (!p) return false;

    uint16_t rrclass = MDNS_FLUSH_CLASS;

    if (g_mdns_ipv4) {
        if (!mdns_pkt_add_a(p, true, g_mdns_fqdn, rrclass, MDNS_TTL_S, g_mdns_ipv4)) return false;
    }

    uint8_t ip6[16];
    if (mdns_current_ipv6(ip6)) {
        if (!mdns_pkt_add_aaaa(p, true, g_mdns_fqdn, rrclass, MDNS_TTL_S, ip6)) return false;
    }

    return true;
}

static bool mdns_add_service_records(mdns_pkt_t *p, const mdns_service_t *s, uint32_t ttl_s, bool goodbye) {
    if (!p) return false;
    if (!s) return false;

    char type[128];
    char inst[256];
    mdns_make_service_type(type, sizeof(type), s->service, s->proto);
    inst[0] = 0;
    if (!s->instance[0] || !s->service[0] || !s->proto[0]) return false;
    string_format_buf(inst, sizeof(inst), "%s._%s._%s.local", s->instance, s->service, s->proto);

    uint16_t ptr_class = DNS_CLASS_IN;
    uint16_t flush_class = MDNS_FLUSH_CLASS;

    uint32_t ttl = goodbye ? 0 : ttl_s;

    if (!mdns_pkt_add_ptr(p, false, DNS_SD_ENUM_SERVICES, ptr_class, ttl, type)) return false;
    if (!mdns_pkt_add_ptr(p, false, type, ptr_class, ttl, inst)) return false;

    if (!goodbye) {
        if (!mdns_pkt_add_srv(p, true, inst, flush_class, ttl_s, s->port, g_mdns_fqdn)) return false;
        if (!mdns_pkt_add_txt(p, true, inst, flush_class, ttl_s, s->txt)) return false;
        if (!mdns_add_host_additionals(p)) return false;
    } else {
        if (!mdns_pkt_add_srv(p, true, inst, flush_class, 0, s->port, g_mdns_fqdn)) return false;
        if (!mdns_pkt_add_txt(p, true, inst, flush_class, 0, s->txt)) return false;
    }

    return true;
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

    for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
        mdns_service_t *s = &g_mdns_services[i];
        if (!s->used) continue;
        if (strncmp(s->instance, instance, (int)sizeof(s->instance)) != 0) continue;
        if (strncmp(s->service, service, (int)sizeof(s->service)) != 0) continue;
        if (strncmp_case(s->proto, proto, true, (int)sizeof(s->proto)) != 0) continue;

        s->active = true;
        s->port = port;
        if (txt) strncpy(s->txt, txt, sizeof(s->txt));
        else s->txt[0] = 0;
        s->announce_left = MDNS_ANNOUNCE_BURST;
        s->goodbye_left = 0;
        s->last_tx_ms = 0;
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

        s->announce_left = MDNS_ANNOUNCE_BURST;
        s->goodbye_left = 0;
        s->last_tx_ms = 0;
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
        s->announce_left = 0;
        s->goodbye_left = MDNS_GOODBYE_BURST;
        s->last_tx_ms = 0;
        return true;
    }

    return false;
}

void mdns_responder_tick_multi(const mdns_tx_target_t *targets, uint32_t target_count) {
    if (!targets) target_count = 0;
    mdns_refresh_identity();
    uint64_t now = get_time();
    for (uint32_t i = 0; i < MDNS_CACHE_MAX; i++) {
        mdns_cache_entry_t *e = &g_mdns_cache[i];
        if (!e->type) continue;
        if (now < e->expire_ms) continue;
        memset(e, 0, sizeof(*e));
    }

    if (!g_mdns_last_keepalive_ms) g_mdns_last_keepalive_ms = now;
    if ((now - g_mdns_last_keepalive_ms) >= MDNS_KEEPALIVE_MS) {
        g_mdns_last_keepalive_ms = now;
        g_mdns_host_announce_left = 1;
        g_mdns_host_last_tx_ms = 0;
        for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
            if (!g_mdns_services[i].used) continue;
            if (!g_mdns_services[i].active) continue;
            g_mdns_services[i].announce_left = 1;
            g_mdns_services[i].last_tx_ms = 0;
        }
    }

    if (g_mdns_host_announce_left) {
        if (!g_mdns_host_last_tx_ms || (now - g_mdns_host_last_tx_ms) >= MDNS_ANNOUNCE_INTERVAL_MS) {
            g_mdns_host_last_tx_ms = now;

            uint8_t pkt[900];
            mdns_pkt_t p;
            if (mdns_pkt_begin(&p, pkt, sizeof(pkt), DNS_FLAG_QR | DNS_FLAG_AA)) {
                bool ok = true;
                uint16_t flush_class = MDNS_FLUSH_CLASS;
                if (g_mdns_ipv4 && !mdns_pkt_add_a(&p, false, g_mdns_fqdn, flush_class, MDNS_TTL_S, g_mdns_ipv4)) ok = false;

                uint8_t ip6[16];
                if (mdns_current_ipv6(ip6) && !mdns_pkt_add_aaaa(&p, false, g_mdns_fqdn, flush_class, MDNS_TTL_S, ip6)) ok = false;

                if (ok && p.an) {
                    mdns_pkt_commit(&p);

                    for (uint32_t t = 0; t < target_count; t++) {
                        if (!targets[t].sock) continue;
                        mdns_send(targets[t].sock, 0, false, targets[t].ver, targets[t].mcast_ip, pkt, p.off);
                    }

                    uint32_t ttl_ms = MDNS_TTL_S * 1000;

                    if (g_mdns_ipv4) {
                        uint8_t ip4[16];
                        memset(ip4, 0, sizeof(ip4));
                        memcpy(ip4, &g_mdns_ipv4, 4);
                        dns_cache_put_ip(g_mdns_fqdn, DNS_TYPE_A, ip4, ttl_ms);
                    }

                    uint8_t ip6_cache[16];
                    if (mdns_current_ipv6(ip6_cache)) dns_cache_put_ip(g_mdns_fqdn, DNS_TYPE_AAAA, ip6_cache, ttl_ms);
                }
            }

            g_mdns_host_announce_left--;
        }
    }

    for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
        mdns_service_t *s = &g_mdns_services[i];
        if (!s->used) continue;

        bool do_goodbye = false;
        if (s->goodbye_left) do_goodbye = true;
        if (!do_goodbye && !s->active) continue;
        if (!do_goodbye && !s->announce_left) continue;

        uint32_t interval = MDNS_ANNOUNCE_INTERVAL_MS;
        if (!s->last_tx_ms || (now - s->last_tx_ms) >= interval) {
            s->last_tx_ms = now;

            uint8_t pkt[900];
            mdns_pkt_t p;
            if (mdns_pkt_begin(&p, pkt, sizeof(pkt), DNS_FLAG_QR | DNS_FLAG_AA) && mdns_add_service_records(&p, s, MDNS_TTL_S, do_goodbye) && (p.an || p.ar)) {
                mdns_pkt_commit(&p);

                for (uint32_t t = 0; t < target_count; t++) {
                    if (!targets[t].sock) continue;
                    mdns_send(targets[t].sock, 0, false, targets[t].ver, targets[t].mcast_ip, pkt, p.off);
                }
            }

            if (do_goodbye) {
                if (s->goodbye_left) s->goodbye_left--;
                if (!s->goodbye_left) {
                    memset(s, 0, sizeof(*s));
                }
            } else {
                if (s->announce_left) s->announce_left--;
            }
        }
    }
}

void mdns_responder_tick(socket_handle_t sock, const uint8_t mcast_v4[4], const uint8_t mcast_v6[16]) {
    mdns_tx_target_t targets[2];
    uint32_t n = 0;

    if (sock && mcast_v4) {
        memset(&targets[n], 0, sizeof(targets[n]));
        targets[n].sock = sock;
        targets[n].ver = IP_VER4;
        memcpy(targets[n].mcast_ip, mcast_v4, 4);
        n++;
    }

    if (sock && mcast_v6) {
        memset(&targets[n], 0, sizeof(targets[n]));
        targets[n].sock = sock;
        targets[n].ver = IP_VER6;
        memcpy(targets[n].mcast_ip, mcast_v6, 16);
        n++;
    }

    mdns_responder_tick_multi(targets, n);
}

void mdns_responder_handle_query(socket_handle_t sock, ip_version_t ver, const uint8_t *mcast_ip, const uint8_t *pkt, uint32_t pkt_len, const net_l4_endpoint *src) {
    if (!sock) return;
    if (!mcast_ip) return;
    if (!pkt) return;
    if (pkt_len < 12) return;

    mdns_refresh_identity();

    uint16_t flags = rd_be16(pkt + 2);
    if (flags & DNS_FLAG_QR) {
        mdns_cache_from_packet(pkt, pkt_len);
        return;
    }

    uint16_t qd = rd_be16(pkt + 4);
    if (!qd) return;

    uint64_t now = get_time();
    uint64_t hash = hash_map_fnv1a64(pkt, pkt_len);
    uint16_t src_port = src ? src->port : 0;
    uint8_t src_ip_zero[16];
    memset(src_ip_zero, 0, sizeof(src_ip_zero));
    const uint8_t *src_ip = src ? src->ip : src_ip_zero;
    bool duplicate_query = false;

    for (uint32_t i = 0; i < MDNS_QUERY_DEDUP_MAX; i++) {
        mdns_query_dedup_t *e = &g_mdns_query_dedup[i];
        if (!e->used || e->ver != ver || e->port != src_port || e->hash != hash) continue;
        if (memcmp(e->ip, src_ip, ver == IP_VER4 ? 4 : 16) != 0) continue;
        if (now - e->last_ms >= MDNS_QUERY_DEDUP_MS) continue;
        e->last_ms = now;
        duplicate_query = true;
        break;
    }

    if (duplicate_query) return;

    mdns_query_dedup_t *dedup = &g_mdns_query_dedup[g_mdns_query_dedup_next++ % MDNS_QUERY_DEDUP_MAX];
    memset(dedup, 0, sizeof(*dedup));
    dedup->used = true;
    dedup->ver = ver;
    dedup->port = src_port;
    dedup->hash = hash;
    dedup->last_ms = now;
    memcpy(dedup->ip, src_ip, ver == IP_VER4 ? 4 : 16);

    bool unicast_any = src && src->port && src->port != DNS_MDNS_PORT;

    uint8_t out[1500];
    mdns_pkt_t p;
    if (!mdns_pkt_begin(&p, out, sizeof(out), DNS_FLAG_QR | DNS_FLAG_AA)) return;

    uint32_t qoff = 12;

    for (uint16_t qi = 0; qi < qd; qi++) {
        char qname[256];
        uint32_t next = 0;
        if (!dns_wire_read_name(pkt, pkt_len, qoff, qname, sizeof(qname), &next)) return;
        if (next + 4 > pkt_len) return;

        uint16_t qtype = rd_be16(pkt + next);
        uint16_t qclass = rd_be16(pkt + next + 2);
        if ((qclass & DNS_CLASS_MASK) != DNS_CLASS_IN && (qclass & DNS_CLASS_MASK) != DNS_CLASS_ANY) {
            qoff = next + 4;
            continue;
        }
        if ((qclass & DNS_CLASS_CACHE_FLUSH) != 0) unicast_any = true;

        uint32_t ipq = 0;
        if ((qtype == DNS_TYPE_PTR || qtype == DNS_TYPE_ANY) && g_mdns_ipv4 && mdns_parse_ipv4_ptr_qname(qname, &ipq) && ipq == g_mdns_ipv4) {
            uint16_t ptr_class = DNS_CLASS_IN;
            uint16_t flush_class = MDNS_FLUSH_CLASS;

            if (!mdns_pkt_add_ptr(&p, false, qname, ptr_class, MDNS_TTL_S, g_mdns_fqdn)) return;
            if (!mdns_pkt_add_a(&p, true, g_mdns_fqdn, flush_class, MDNS_TTL_S, g_mdns_ipv4)) return;

            uint8_t ip6[16];
            if (mdns_current_ipv6(ip6)) {
                if (!mdns_pkt_add_aaaa(&p, true, g_mdns_fqdn, flush_class, MDNS_TTL_S, ip6)) return;
            }
        }

        if (qtype == DNS_TYPE_A || qtype == DNS_TYPE_ANY) {
            if (dns_wire_name_equals(qname, g_mdns_fqdn) && g_mdns_ipv4) {
                uint16_t flush_class = MDNS_FLUSH_CLASS;
                if (!mdns_pkt_add_a(&p, false, g_mdns_fqdn, flush_class, MDNS_TTL_S, g_mdns_ipv4)) return;
            }
        }

        if (qtype == DNS_TYPE_AAAA || qtype == DNS_TYPE_ANY) {
            if (dns_wire_name_equals(qname, g_mdns_fqdn)) {
                uint8_t ip6[16];
                if (mdns_current_ipv6(ip6)) {
                    uint16_t flush_class = MDNS_FLUSH_CLASS;
                    if (!mdns_pkt_add_aaaa(&p, false, g_mdns_fqdn, flush_class, MDNS_TTL_S, ip6)) return;
                }
            }
        }

        if ((qtype == DNS_TYPE_PTR || qtype == DNS_TYPE_ANY) && dns_wire_name_equals(qname, DNS_SD_ENUM_SERVICES)) {
            uint16_t ptr_class = DNS_CLASS_IN;

            for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
                if (!g_mdns_services[i].used) continue;
                if (!g_mdns_services[i].active) continue;

                char type[128];
                mdns_make_service_type(type,sizeof(type), g_mdns_services[i].service, g_mdns_services[i].proto);

                bool seen = false;
                for (uint32_t j = 0; j < i; j++) {
                    if (!g_mdns_services[j].used) continue;
                    if (!g_mdns_services[j].active) continue;

                    char type2[128];
                    mdns_make_service_type(type2,sizeof(type2),g_mdns_services[j].service, g_mdns_services[j].proto);
                    if (dns_wire_name_equals(type2, type)) {
                        seen = true;
                        break;
                    }
                }

                if (seen) continue;
                if (!mdns_pkt_add_ptr(&p, false, DNS_SD_ENUM_SERVICES, ptr_class, MDNS_TTL_S, type)) return;
            }
        }

        bool need_host_add = false;
        for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
            if (!g_mdns_services[i].used) continue;
            if (!g_mdns_services[i].active) continue;

            mdns_service_t *s = &g_mdns_services[i];

            char type[128];
            char inst[256];
            mdns_make_service_type(type, sizeof(type), s->service, s->proto);
            inst[0] = 0;
            if (!s->instance[0] || !s->service[0] || !s->proto[0]) continue;
            string_format_buf(inst, sizeof(inst), "%s._%s._%s.local", s->instance, s->service, s->proto);

            if ((qtype == DNS_TYPE_PTR || qtype == DNS_TYPE_ANY) && dns_wire_name_equals(qname, type)) {
                uint16_t ptr_class = DNS_CLASS_IN;
                if (!mdns_pkt_add_ptr(&p, false, type, ptr_class, MDNS_TTL_S, inst)) return;

                uint16_t flush_class = MDNS_FLUSH_CLASS;
                if (!mdns_pkt_add_srv(&p, true, inst, flush_class, MDNS_TTL_S, s->port, g_mdns_fqdn)) return;
                if (!mdns_pkt_add_txt(&p, true, inst, flush_class, MDNS_TTL_S, s->txt)) return;
                need_host_add = true;
            }

            if ((qtype == DNS_TYPE_SRV || qtype == DNS_TYPE_ANY) && dns_wire_name_equals(qname, inst)) {
                uint16_t flush_class = MDNS_FLUSH_CLASS;
                if (!mdns_pkt_add_srv(&p, false, inst, flush_class, MDNS_TTL_S, s->port, g_mdns_fqdn)) return;
                need_host_add = true;
            }

            if ((qtype == DNS_TYPE_TXT || qtype ==  DNS_TYPE_ANY) && dns_wire_name_equals(qname, inst)) {
                uint16_t flush_class = MDNS_FLUSH_CLASS;
                if (!mdns_pkt_add_txt(&p, false, inst, flush_class, MDNS_TTL_S, s->txt)) return;
            }
        }

        if (need_host_add) {
            if (!mdns_add_host_additionals(&p)) return;
        }

        qoff = next + 4;
        if (qoff > pkt_len) return;
    }

    if (!p.an && !p.ar) return;

    mdns_pkt_commit(&p);
    mdns_send(sock, src, unicast_any, ver, mcast_ip, out, p.off);
    if (unicast_any) mdns_send(sock, NULL, false, ver, mcast_ip, out, p.off);

    uint32_t ttl_ms = MDNS_TTL_S * 1000;

    if (g_mdns_ipv4) {
        uint8_t ip4[16];
        memset(ip4, 0, sizeof(ip4));
        memcpy(ip4, &g_mdns_ipv4, 4);
        dns_cache_put_ip(g_mdns_fqdn, DNS_TYPE_A, ip4, ttl_ms);
    }

    uint8_t ip6[16];
    if (mdns_current_ipv6(ip6)) dns_cache_put_ip(g_mdns_fqdn, DNS_TYPE_AAAA, ip6, ttl_ms);
}