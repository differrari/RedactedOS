#include "mdns_responder.h"

#include "dns_sd.h"
#include "dns_cache.h"
#include "net/internet_layer/ipv6_utils.h"
#include "net/transport_layer/csocket_udp.h"
#include "networking/interface_manager.h"
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
    uint16_t rrtype;
    uint16_t rrclass;
    uint32_t ttl_s;
    uint16_t rdlen;
} mdns_rr_hdr_t;

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


static bool mdns_read_name(const uint8_t *msg, uint32_t msg_len, uint32_t off, char *out, uint32_t out_cap, uint32_t *out_next) {
    if (!msg) return false;
    if (!msg_len) return false;
    if (off >= msg_len) return false;
    if (!out) return false;
    if (!out_cap) return false;

    uint32_t idx = off;
    uint32_t out_idx = 0;
    uint32_t jumps = 0;
    bool jumped = false;

    while (true) {
        if (idx >= msg_len) return false;

        uint8_t c = msg[idx];
        if ((c & 0xC0) == 0xC0) {
            if (idx + 1 >= msg_len) return false;
            uint16_t ptr = (uint16_t)(((uint16_t)(c & 0x3F) << 8) | msg[idx + 1]);
            if (ptr >= msg_len) return false;
            if (!jumped) {
                if (out_next) *out_next = idx + 2;
                jumped = true;
            }
            idx = ptr;
            jumps++;
            if (jumps > 16) return false;
            continue;
        }

        if (c == 0) {
            if (!jumped) {
                if (out_next) *out_next = idx + 1;
            }
            if (out_idx == 0) {
                if (out_cap < 2) return false;
                out[0] = '.';
                out[1] = 0;
                return true;
            }
            if (out_idx >= out_cap) return false;
            out[out_idx] = 0;
            return true;
        }

        uint32_t lab_len = c;
        idx++;
        if (idx + lab_len > msg_len) return false;

        if (out_idx) {
            if (out_idx + 1 >= out_cap) return false;
            out[out_idx] = '.';
            out_idx++;
        }

        if (out_idx + lab_len >= out_cap) return false;
        memcpy(out + out_idx, msg + idx, lab_len);
        out_idx += lab_len;
        idx += lab_len;
    }
}

static void mdns_send(socket_handle_t sock, const net_l4_endpoint *src, bool unicast, ip_version_t ver, const uint8_t *mcast_ip, const uint8_t *pkt, uint32_t pkt_len) {
    if (!sock) return;
    if (!pkt) return;
    if (!pkt_len) return;

    net_l4_endpoint dst;
    memset(&dst, 0, sizeof(dst));

    if (unicast && src) {
        dst = *src;
        if (!dst.port) dst.port = DNS_SD_MDNS_PORT;
        socket_sendto_udp_ex(sock, DST_ENDPOINT, &dst, 0, (void*)pkt, pkt_len);
        return;
    }

    dst.ver = ver;
    if (ver == IP_VER4) memcpy(dst.ip, mcast_ip, 4);
    else memcpy(dst.ip, mcast_ip, 16);
    dst.port = DNS_SD_MDNS_PORT;
    socket_sendto_udp_ex(sock, DST_ENDPOINT, &dst, 0, (void*)pkt, pkt_len);
}

static bool mdns_pick_identity(uint32_t *out_v4, uint8_t out_v6[16], uint8_t *out_ifindex, uint8_t out_ifid[8]) {
    if (!out_v4) return false;
    if (!out_v6) return false;
    if (!out_ifindex) return false;
    if (!out_ifid) return false;

    uint32_t v4 = 0;
    uint8_t v6_best[16];
    uint8_t v6_fallback[16];
    uint8_t ifid_best[8];
    uint8_t ifid_fallback[8];
    uint8_t if_best = 0;
    uint8_t if_fallback = 0;

    memset(v6_best, 0, sizeof(v6_best));
    memset(v6_fallback, 0, sizeof(v6_fallback));
    memset(ifid_best, 0, sizeof(ifid_best));
    memset(ifid_fallback, 0, sizeof(ifid_fallback));

    uint8_t c = l2_interface_count();
    for (uint8_t i = 0; i < c; i++) {
        l2_interface_t *l2 = l2_interface_at(i);
        if (!l2)continue;
        if (!l2->is_up) continue;

        if (!v4) {
            for (uint8_t j = 0; j < l2->ipv4_count; j++) {
                l3_ipv4_interface_t *a = l2->l3_v4[j];
                if (!a) continue;
                if (a->is_localhost) continue;
                if (!a->ip) continue;
                v4 = a->ip;
                break;
            }
        }

        for (uint8_t j = 0; j < l2->ipv6_count; j++) {
            l3_ipv6_interface_t *a = l2->l3_v6[j];
            if (!a) continue;
            if (a->is_localhost) continue;
            if (!a->ip[0]) continue;

            bool is_lla = (a->ip[0] == 0xFE && (a->ip[1] & 0xC0) == 0x80);
            if (!is_lla && !if_best) {
                memcpy(v6_best, a->ip, 16);
                memcpy(ifid_best, a->interface_id, 8);
                if_best = l2->ifindex;
            }

            if (!if_fallback) {
                memcpy(v6_fallback, a->ip, 16);
                memcpy(ifid_fallback,a->interface_id, 8);
                if_fallback = l2->ifindex;
            }
        }
    }

    if (if_best) {
        *out_v4 = v4;
        memcpy(out_v6, v6_best, 16);
        *out_ifindex = if_best;
        memcpy(out_ifid, ifid_best, 8);
        return true;
    }

    if (if_fallback) {
        *out_v4 = v4;
        memcpy(out_v6, v6_fallback, 16);
        *out_ifindex = if_fallback;
        memcpy(out_ifid, ifid_fallback, 8);
        return true;
    }

    if (v4) {
        *out_v4 = v4;
        memset(out_v6, 0, 16);
        *out_ifindex = 0;
        memset(out_ifid, 0, 8);
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
    uint8_t ifid[8];
    uint8_t ifindex = 0;
    memset(v6, 0, sizeof(v6));
    memset(ifid, 0, sizeof(ifid));

    if (!mdns_pick_identity(&v4, v6, &ifindex, ifid)) return;

    bool changed = false;
    if (g_mdns_ipv4 != v4) changed = true;
    if (memcmp(g_mdns_ipv6, v6, 16) != 0) changed = true;
    if (g_mdns_ifindex != ifindex) changed = true;

    g_mdns_ipv4 = v4;
    memcpy(g_mdns_ipv6, v6, 16);
    g_mdns_ifindex = ifindex;

    if (!g_mdns_fqdn[0]) {
        char host[64];
        string_format_buf(host, sizeof(host),"redactedos-%02x%02x%02x%02x%02x%02x%02x%02x", ifid[0],ifid[1],ifid[2],ifid[3],ifid[4],ifid[5], ifid[6], ifid[7]);
        string_format_buf(g_mdns_fqdn, sizeof(g_mdns_fqdn), "%s.local", host);
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

    p->off = dns_sd_put_u16(out, cap, p->off, 0);
    if (!p->off) return false;
    p->off = dns_sd_put_u16(out, cap, p->off, flags);
    if (!p->off) return false;
    p->off = dns_sd_put_u16(out, cap, p->off, 0);
    if (!p->off) return false;

    p->an_pos = p->off;
    p->off = dns_sd_put_u16(out, cap, p->off, 0);
    if (!p->off) return false;

    p->off = dns_sd_put_u16(out, cap, p->off, 0);
    if (!p->off) return false;

    p->ar_pos = p->off;
    p->off = dns_sd_put_u16(out, cap, p->off, 0);
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


static void mdns_cache_put_ptr(const char *name, const char *target, uint32_t ttl_s) {
    if (!name) return;
    if (!target) return;

    uint64_t now = get_time();
    for (uint32_t i = 0; i < MDNS_CACHE_MAX; i++) {
        mdns_cache_entry_t *e = &g_mdns_cache[i];
        if (!e->type) continue;
        if (e->rrtype != DNS_SD_TYPE_PTR) continue;
        if (strncmp(e->name, name, true, 256) != 0) continue;
        strncpy(e->target, sizeof(e->target), target);
        e->expire_ms = now + (uint64_t)ttl_s * 1000;
        return;
    }

    for (uint32_t i = 0; i < MDNS_CACHE_MAX; i++) {
        mdns_cache_entry_t *e = &g_mdns_cache[i];
        if (e->type) continue;
        memset(e, 0, sizeof(*e));
        e->type = 1;
        e->rrtype = DNS_SD_TYPE_PTR;
        strncpy(e->name, sizeof(e->name), name);
        strncpy(e->target, sizeof(e->target), target);
        e->expire_ms = now + (uint64_t)ttl_s * 1000;
        return;
    }
}

static void mdns_cache_put_srv(const char *name, uint16_t port, const char *target, uint32_t ttl_s) {
    if (!name) return;
    if (!target) return;

    uint64_t now = get_time();
    for (uint32_t i = 0; i < MDNS_CACHE_MAX; i++) {
        mdns_cache_entry_t *e = &g_mdns_cache[i];
        if (!e->type) continue;
        if (e->rrtype != DNS_SD_TYPE_SRV) continue;
        if (strncmp(e->name, name, true, 256) != 0) continue;
        e->port = port;
        strncpy(e->target, sizeof(e->target), target);
        e->expire_ms = now + (uint64_t)ttl_s * 1000;
        return;
    }

    for (uint32_t i = 0; i < MDNS_CACHE_MAX; i++) {
        mdns_cache_entry_t *e = &g_mdns_cache[i];
        if (e->type) continue;
        memset(e, 0, sizeof(*e));
        e->type = 1;
        e->rrtype = DNS_SD_TYPE_SRV;
        e->port = port;
        strncpy(e->name, sizeof(e->name), name);
        strncpy(e->target, sizeof(e->target), target);
        e->expire_ms = now + (uint64_t)ttl_s * 1000;
        return;
    }
}

static void mdns_cache_put_txt(const char *name, const char *txt, uint32_t ttl_s) {
    if (!name) return;

    uint64_t now = get_time();
    for (uint32_t i = 0; i < MDNS_CACHE_MAX; i++) {
        mdns_cache_entry_t *e = &g_mdns_cache[i];
        if (!e->type) continue;
        if (e->rrtype != DNS_SD_TYPE_TXT) continue;
        if (strncmp(e->name, name, true, 256) != 0) continue;
        if (txt) strncpy(e->txt, sizeof(e->txt), txt);
        else e->txt[0] = 0;
        e->expire_ms = now + (uint64_t)ttl_s * 1000;
        return;
    }

    for (uint32_t i = 0; i < MDNS_CACHE_MAX; i++) {
        mdns_cache_entry_t *e = &g_mdns_cache[i];
        if (e->type) continue;
        memset(e, 0, sizeof(*e));
        e->type = 1;
        e->rrtype = DNS_SD_TYPE_TXT;
        strncpy(e->name, sizeof(e->name), name);
        if (txt) strncpy(e->txt, sizeof(e->txt), txt);
        e->expire_ms = now + (uint64_t)ttl_s * 1000;
        return;
    }
}

static bool mdns_parse_ipv4_ptr_qname(const char *name, uint32_t *out_ip) {
    if (!name) return false;
    if (!out_ip) return false;

    uint32_t oct[4];
    memset(oct, 0, sizeof(oct));

    const char *p = name;
    for (int i = 0; i < 4; i++) {
        uint32_t v = 0;
        uint32_t digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (uint32_t)(*p - '0');
            p++;
            digits++;
            if (digits > 3) return false;
        }
        if (!digits) return false;
        if (v > 255) return false;
        oct[i] = v;
        if (i < 3) {
            if (*p != '.') return false;
            p++;
        }
    }

    if (*p != '.') return false;
    p++;

    if (strncmp(p, "in-addr.arpa", true, 12) != 0) return false;

    uint32_t a = oct[3];
    uint32_t b = oct[2];
    uint32_t c = oct[1];
    uint32_t d = oct[0];

    *out_ip = (uint32_t)((a & 255u) | ((b & 255u) << 8) | ((c & 255u) << 16) | ((d & 255u) << 24));
    return true;
}

static void mdns_parse_txt(const uint8_t *rdata, uint16_t rdlen, char *out, uint32_t out_cap) {
    if (!out) return;
    if (!out_cap) return;

    out[0] = 0;
    if (!rdata) return;
    if (!rdlen) return;

    uint32_t idx = 0;
    uint32_t out_idx = 0;

    while (idx < rdlen) {
        uint8_t len = rdata[idx];
        idx++;
        if (idx + len > rdlen) break;

        if (len) {
            if (out_idx) {
                if (out_idx + 1 >= out_cap) break;
                out[out_idx] = ';';
                out_idx++;
            }
            uint32_t copy = len;
            if (out_idx + copy >= out_cap) copy = out_cap - out_idx - 1;
            memcpy(out + out_idx, rdata + idx, copy);
            out_idx += copy;
        }

        idx += len;
    }

    if (out_idx >= out_cap) out_idx = out_cap - 1;
    out[out_idx] = 0;
}

static void mdns_cache_from_packet(const uint8_t *pkt, uint32_t pkt_len) {
    if (!pkt) return;
    if (pkt_len < 12) return;

    uint16_t flags = be16(*(const uint16_t *)(pkt + 2));
    if (!(flags & DNS_SD_FLAG_QR)) return;

    uint16_t qd = be16(*(const uint16_t *)(pkt + 4));
    uint16_t an = be16(*(const uint16_t *)(pkt + 6));
    uint16_t ns = be16(*(const uint16_t *)(pkt + 8));
    uint16_t ar = be16(*(const uint16_t *)(pkt + 10));

    uint32_t off = 12;

    for (uint16_t i = 0; i < qd; i++) {
        char qname[256];
        uint32_t next = 0;
        if (!mdns_read_name(pkt, pkt_len, off, qname, sizeof(qname), &next)) return;
        if (next + 4 > pkt_len) return;
        off = next + 4;
        if (off > pkt_len) return;
    }

    uint32_t rr_total = (uint32_t)an + (uint32_t)ns + (uint32_t)ar;
    for (uint32_t i = 0; i < rr_total; i++) {
        char name[256];
        uint32_t next = 0;
        if (!mdns_read_name(pkt, pkt_len, off, name, sizeof(name), &next)) return;
        if (next + 10 > pkt_len) return;

        mdns_rr_hdr_t h;
        h.rrtype = be16(*(const uint16_t *)(pkt + next));
        h.rrclass = be16(*(const uint16_t *)(pkt + next + 2));
        h.ttl_s = be32(*(const uint32_t *)(pkt + next + 4));
        h.rdlen = be16(*(const uint16_t *)(pkt + next + 8));

        uint32_t rdata = next + 10;
        if (rdata + h.rdlen > pkt_len) return;

        if (h.rrtype == DNS_SD_TYPE_A) {
            if (h.rdlen == 4) {
                uint8_t ip4[16];
                memset(ip4, 0, sizeof(ip4));
                memcpy(ip4, pkt + rdata, 4);
                dns_cache_put_ip(name, DNS_SD_TYPE_A, ip4, h.ttl_s * 1000);
            }
        } else if (h.rrtype == DNS_SD_TYPE_AAAA) {
            if (h.rdlen == 16) {
                dns_cache_put_ip(name, DNS_SD_TYPE_AAAA, pkt + rdata, h.ttl_s * 1000);
            }
        } else if (h.rrtype == DNS_SD_TYPE_PTR) {
            char target[256];
            uint32_t tnext = 0;
            if (mdns_read_name(pkt, pkt_len, rdata, target, sizeof(target), &tnext)) {
                mdns_cache_put_ptr(name, target, h.ttl_s);
            }
        } else if (h.rrtype == DNS_SD_TYPE_SRV) {
            if (h.rdlen >= 6) {
                uint16_t port = be16(*(const uint16_t *)(pkt + rdata + 4));
                char target[256];
                uint32_t tnext = 0;
                if (mdns_read_name(pkt, pkt_len, rdata + 6, target, sizeof(target), &tnext)) {
                    mdns_cache_put_srv(name, port, target, h.ttl_s);
                }
            }
        } else if (h.rrtype == DNS_SD_TYPE_TXT) {
            char txt[256];
            mdns_parse_txt(pkt + rdata, h.rdlen, txt, sizeof(txt));
            mdns_cache_put_txt(name, txt, h.ttl_s);
        }

        off = rdata + h.rdlen;
        if (off > pkt_len) return;
    }
}

static bool mdns_add_host_additionals(mdns_pkt_t *p) {
    if (!p) return false;

    uint16_t rrclass = (uint16_t)(0x8000u | DNS_SD_CLASS_IN);

    if (g_mdns_ipv4) {
        if (!mdns_pkt_add_a(p, true, g_mdns_fqdn, rrclass, MDNS_TTL_S, g_mdns_ipv4)) return false;
    }

    uint8_t ip6[16];
    memcpy(ip6, g_mdns_ipv6, 16);
    if (!ip6[0] && g_mdns_ifindex) ipv6_make_lla_from_mac(g_mdns_ifindex, ip6);
    if (ip6[0]) {
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

    uint16_t ptr_class = DNS_SD_CLASS_IN;
    uint16_t flush_class = (uint16_t)(0x8000u | DNS_SD_CLASS_IN);

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

bool mdns_register_service(const char *instance, const char *service, const char *proto, uint16_t port, const char *txt) {
    if (!instance) return false;
    if (!service) return false;
    if (!proto) return false;

    for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
        mdns_service_t *s = &g_mdns_services[i];
        if (!s->used) continue;
        if (strncmp(s->instance, instance, true, (int)sizeof(s->instance)) != 0) continue;
        if (strncmp(s->service, service, true, (int)sizeof(s->service)) != 0) continue;
        if (strncmp(s->proto, proto, true, (int)sizeof(s->proto)) != 0) continue;

        s->active = true;
        s->port = port;
        if (txt) strncpy(s->txt, sizeof(s->txt), txt);
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

        strncpy(s->instance, sizeof(s->instance), instance);
        strncpy(s->service, sizeof(s->service), service);
        strncpy(s->proto, sizeof(s->proto), proto);
        if (txt) strncpy(s->txt, sizeof(s->txt), txt);

        s->announce_left = MDNS_ANNOUNCE_BURST;
        s->goodbye_left = 0;
        s->last_tx_ms = 0;
        return true;
    }

    return false;
}

bool mdns_deregister_service(const char *instance, const char *service, const char *proto) {
    if (!instance) return false;
    if (!service) return false;
    if (!proto) return false;

    for (uint32_t i = 0; i < MDNS_MAX_SERVICES; i++) {
        mdns_service_t *s = &g_mdns_services[i];
        if (!s->used) continue;
        if (!s->active) continue;
        if (strncmp(s->instance, instance, true, (int)sizeof(s->instance)) != 0) continue;
        if (strncmp(s->service, service, true, (int)sizeof(s->service)) != 0) continue;
        if (strncmp(s->proto, proto, true, (int)sizeof(s->proto)) != 0) continue;

        s->active = false;
        s->announce_left = 0;
        s->goodbye_left = MDNS_GOODBYE_BURST;
        s->last_tx_ms = 0;
        return true;
    }

    return false;
}

void mdns_responder_tick(socket_handle_t sock4, socket_handle_t sock6, const uint8_t mcast_v4[4], const uint8_t mcast_v6[16]) {
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
            if (mdns_pkt_begin(&p, pkt, sizeof(pkt), (uint16_t)(DNS_SD_FLAG_QR | DNS_SD_FLAG_AA))) {
                uint16_t flush_class = (uint16_t)(0x8000u | DNS_SD_CLASS_IN);
                if (g_mdns_ipv4) mdns_pkt_add_a(&p, false, g_mdns_fqdn, flush_class, MDNS_TTL_S, g_mdns_ipv4);

                uint8_t ip6[16];
                memcpy(ip6, g_mdns_ipv6, 16);
                if (!ip6[0] && g_mdns_ifindex) ipv6_make_lla_from_mac(g_mdns_ifindex, ip6);
                if (ip6[0]) mdns_pkt_add_aaaa(&p, false, g_mdns_fqdn, flush_class, MDNS_TTL_S, ip6);

                mdns_pkt_commit(&p);

                if (sock4 && mcast_v4) mdns_send(sock4, 0, false, IP_VER4, mcast_v4, pkt, p.off);
                if (sock6 && mcast_v6) mdns_send(sock6, 0, false, IP_VER6, mcast_v6, pkt, p.off);

                uint32_t ttl_ms = MDNS_TTL_S * 1000;

                if (g_mdns_ipv4) {
                    uint8_t ip4[16];
                    memset(ip4, 0, sizeof(ip4));
                    memcpy(ip4, &g_mdns_ipv4, 4);
                    dns_cache_put_ip(g_mdns_fqdn, DNS_SD_TYPE_A, ip4, ttl_ms);
                }

                uint8_t ip6_cache[16];
                memcpy(ip6_cache, g_mdns_ipv6, 16);
                if (!ip6_cache[0] && g_mdns_ifindex) ipv6_make_lla_from_mac(g_mdns_ifindex, ip6_cache);
                if (ip6_cache[0]) dns_cache_put_ip(g_mdns_fqdn, DNS_SD_TYPE_AAAA, ip6_cache, ttl_ms);
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
            if (mdns_pkt_begin(&p, pkt, sizeof(pkt), (uint16_t)(DNS_SD_FLAG_QR | DNS_SD_FLAG_AA))) {
                mdns_add_service_records(&p, s, MDNS_TTL_S, do_goodbye);
                mdns_pkt_commit(&p);

                if (sock4 && mcast_v4) mdns_send(sock4, 0, false, IP_VER4, mcast_v4, pkt, p.off);
                if (sock6 && mcast_v6) mdns_send(sock6, 0, false, IP_VER6, mcast_v6, pkt, p.off);
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

void mdns_responder_handle_query(socket_handle_t sock, ip_version_t ver, const uint8_t *mcast_ip, const uint8_t *pkt, uint32_t pkt_len, const net_l4_endpoint *src) {
    if (!sock) return;
    if (!mcast_ip) return;
    if (!pkt) return;
    if (pkt_len < 12) return;

    mdns_refresh_identity();

    uint16_t flags = be16(*(const uint16_t *)(pkt + 2));
    if (flags & DNS_SD_FLAG_QR) {
        mdns_cache_from_packet(pkt, pkt_len);
        return;
    }

    uint16_t qd = be16(*(const uint16_t *)(pkt + 4));
    if (!qd) return;

    bool unicast_any = false;

    uint8_t out[1500];
    mdns_pkt_t p;
    if (!mdns_pkt_begin(&p, out, sizeof(out), (uint16_t)(DNS_SD_FLAG_QR | DNS_SD_FLAG_AA))) return;

    uint32_t qoff = 12;

    for (uint16_t qi = 0; qi < qd; qi++) {
        char qname[256];
        uint32_t next = 0;
        if (!mdns_read_name(pkt, pkt_len, qoff, qname, sizeof(qname), &next)) return;
        if (next + 4 > pkt_len) return;

        uint16_t qtype = be16(*(const uint16_t *)(pkt + next));
        uint16_t qclass = be16(*(const uint16_t *)(pkt + next + 2));
        if ((qclass & 0x8000u) != 0) unicast_any = true;

        uint32_t ipq = 0;
        if (qtype == DNS_SD_TYPE_PTR && g_mdns_ipv4 && mdns_parse_ipv4_ptr_qname(qname, &ipq) && ipq == g_mdns_ipv4) {
            uint16_t ptr_class = DNS_SD_CLASS_IN;
            uint16_t flush_class = (uint16_t)(0x8000u | DNS_SD_CLASS_IN);

            if (!mdns_pkt_add_ptr(&p, false, qname, ptr_class, MDNS_TTL_S, g_mdns_fqdn)) return;
            if (!mdns_pkt_add_a(&p, true, g_mdns_fqdn, flush_class, MDNS_TTL_S, g_mdns_ipv4)) return;

            uint8_t ip6[16];
            memcpy(ip6, g_mdns_ipv6, 16);
            if (!ip6[0] && g_mdns_ifindex) ipv6_make_lla_from_mac(g_mdns_ifindex, ip6);
            if (ip6[0]) {
                if (!mdns_pkt_add_aaaa(&p, true, g_mdns_fqdn, flush_class, MDNS_TTL_S, ip6)) return;
            }
        }

        if (qtype == DNS_SD_TYPE_A || qtype == DNS_SD_TYPE_ANY) {
            if (strncmp(qname, g_mdns_fqdn, true, 256) == 0 && g_mdns_ipv4) {
                uint16_t flush_class = (uint16_t)(0x8000u | DNS_SD_CLASS_IN);
                if (!mdns_pkt_add_a(&p, false, g_mdns_fqdn, flush_class, MDNS_TTL_S, g_mdns_ipv4)) return;
            }
        }

        if (qtype == DNS_SD_TYPE_AAAA || qtype == DNS_SD_TYPE_ANY) {
            if (strncmp(qname, g_mdns_fqdn, true, 256) == 0) {
                uint8_t ip6[16];
                memcpy(ip6, g_mdns_ipv6, 16);
                if (!ip6[0] && g_mdns_ifindex) ipv6_make_lla_from_mac(g_mdns_ifindex, ip6);
                if (ip6[0]) {
                    uint16_t flush_class = (uint16_t)(0x8000u | DNS_SD_CLASS_IN);
                    if (!mdns_pkt_add_aaaa(&p, false, g_mdns_fqdn, flush_class, MDNS_TTL_S, ip6)) return;
                }
            }
        }

        if ((qtype == DNS_SD_TYPE_PTR || qtype == DNS_SD_TYPE_ANY) &&
            strncmp(qname, DNS_SD_ENUM_SERVICES, true, 256) == 0) {
            uint16_t ptr_class = DNS_SD_CLASS_IN;

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
                    if (strncmp(type2, type, true, 128) == 0) {
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

            if ((qtype == DNS_SD_TYPE_PTR || qtype == DNS_SD_TYPE_ANY) &&
                strncmp(qname, type, true, 256) == 0) {
                uint16_t ptr_class = DNS_SD_CLASS_IN;
                if (!mdns_pkt_add_ptr(&p, false, type, ptr_class, MDNS_TTL_S, inst)) return;

                uint16_t flush_class = (uint16_t)(0x8000u | DNS_SD_CLASS_IN);
                if (!mdns_pkt_add_srv(&p, true, inst, flush_class, MDNS_TTL_S, s->port, g_mdns_fqdn)) return;
                if (!mdns_pkt_add_txt(&p, true, inst, flush_class, MDNS_TTL_S, s->txt)) return;
                need_host_add = true;
            }

            if ((qtype == DNS_SD_TYPE_SRV || qtype == DNS_SD_TYPE_ANY) &&
                strncmp(qname, inst, true, 256) == 0) {
                uint16_t flush_class = (uint16_t)(0x8000u | DNS_SD_CLASS_IN);
                if (!mdns_pkt_add_srv(&p, false, inst, flush_class, MDNS_TTL_S, s->port, g_mdns_fqdn)) return;
                need_host_add = true;
            }

            if ((qtype == DNS_SD_TYPE_TXT || qtype == DNS_SD_TYPE_ANY) &&
                strncmp(qname, inst, true, 256) == 0) {
                uint16_t flush_class = (uint16_t)(0x8000u | DNS_SD_CLASS_IN);
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

    uint32_t ttl_ms = MDNS_TTL_S * 1000;

    if (g_mdns_ipv4) {
        uint8_t ip4[16];
        memset(ip4, 0, sizeof(ip4));
        memcpy(ip4, &g_mdns_ipv4, 4);
        dns_cache_put_ip(g_mdns_fqdn, DNS_SD_TYPE_A, ip4, ttl_ms);
    }

    uint8_t ip6[16];
    memcpy(ip6, g_mdns_ipv6, 16);
    if (!ip6[0] && g_mdns_ifindex) ipv6_make_lla_from_mac(g_mdns_ifindex, ip6);
    if (ip6[0]) dns_cache_put_ip(g_mdns_fqdn, DNS_SD_TYPE_AAAA, ip6, ttl_ms);
}