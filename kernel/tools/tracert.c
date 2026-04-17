#include "tracert.h"
#include "networking/internet_layer/icmp.h"
#include "net/network_types.h"
#include "std/string.h"
#include "std/memory.h"
#include "types.h"
#include "console/kio.h"
#include "filesystem/filesystem.h"
#include "process/scheduler.h"
#include "syscalls/syscalls.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv4_route.h"
#include "networking/application_layer/dns/dns.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/internet_layer/icmpv6.h"

typedef struct {
    ip_version_t ver;
    uint32_t max_ttl;
    uint32_t count;
    uint32_t timeout_ms;
    uint32_t interval_ms;
    uint32_t src_ip;
    uint32_t timeout_streak_limit;
    bool src_set;
    const char *host;
} tr_opts_t;

static bool parse_args(int argc, char *argv[], tr_opts_t *o) {
    o->ver = IP_VER4;
    o->max_ttl = 30;
    o->count = 3;
    o->timeout_ms = 1000;
    o->interval_ms = 250;
    o->src_ip = 0;
    o->timeout_streak_limit = 5;
    o->src_set = false;
    o->host = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a && a[0] == '-') {
            if (strcmp_case(a, "-4",true) == 0) {
                o->ver = IP_VER4;
            } else if (strcmp_case(a, "-6",true) == 0) {
                o->ver = IP_VER6;
            } else if (strcmp_case(a, "-m",true) == 0) {
                if (++i >= argc) return false;
                if (!parse_uint32_dec(argv[i], &o->max_ttl) || o->max_ttl == 0) return false;
            } else if (strcmp_case(a, "-n",true) == 0) {
                if (++i >= argc) return false;
                if (!parse_uint32_dec(argv[i], &o->count) || o->count == 0) return false;
            } else if (strcmp_case(a, "-w",true) == 0) {
                if (++i >= argc) return false;
                if (!parse_uint32_dec(argv[i], &o->timeout_ms) || o->timeout_ms == 0) return false;
            } else if (strcmp_case(a, "-i",true) == 0) {
                if (++i >= argc) return false;
                if (!parse_uint32_dec(argv[i], &o->interval_ms)) return false;
            } else if (strcmp_case(a, "-x",true) == 0) {
                if (++i >= argc) return false;
                if (!parse_uint32_dec(argv[i], &o->timeout_streak_limit) || o->timeout_streak_limit == 0) return false;
            } else if (strcmp_case(a, "-s",true) == 0) {
                if (++i >= argc) return false;
                uint32_t src = 0;
                if (!ipv4_parse(argv[i], &src)) return false;
                o->src_ip = src;
                o->src_set = true;
            } else {
                return false;
            }
        } else {
            if (o->host) return false;
            o->host = a;
        }
    }

    if (!o->host) return false;
    if (o->max_ttl == 0 || o->max_ttl > 64) o->max_ttl = 30;
    if (o->count == 0 || o->count > 5) o->count = 3;
    if (o->timeout_ms < 100) o->timeout_ms = 100;
    if (o->timeout_ms > 5000) o->timeout_ms = 1000;
    if (o->interval_ms > 2000) o->interval_ms = 250;
    if (o->timeout_streak_limit == 0 || o->timeout_streak_limit > 10) o->timeout_streak_limit = 5;
    return true;
}

static int tracert_v4(const tr_opts_t *o) {
    uint32_t dst = 0;
    bool lit = ipv4_parse(o->host, &dst);
    if (!lit) {
        uint32_t r = 0;
        dns_result_t dr = dns_resolve_a(o->host, &r, DNS_USE_BOTH, o->timeout_ms);
        if (dr != DNS_OK) {
            print("tracert: dns lookup failed (%d) for '%s'\n", (int)dr, o->host);
            return 2;
        }
        dst = r;
    }

    char dip[16];
    char line[256];
    ipv4_to_string(dst, dip);
    print("Tracing route to %s [%s]\n", o->host, dip);
    size_t len = string_format_buf(line, sizeof(line), "hop  ");
    for (uint32_t p = 0; p < o->count && len < sizeof(line); p++) len += string_format_buf(line + len, sizeof(line) - len, "rtt%u  ", p + 1);
    string_format_buf(line + len, sizeof(line) - len, "address");
    print("%s\n", line);

    ipv4_tx_opts_t txo = (ipv4_tx_opts_t){0};
    const ipv4_tx_opts_t *txop = NULL;
    if (o->src_set) {
        l3_ipv4_interface_t *l3 = l3_ipv4_find_by_ip(o->src_ip);
        if (!l3) {
            char ssrc[16];
            ipv4_to_string(o->src_ip, ssrc);
            print("tracert: invalid source %s (no local ip match)\n", ssrc);
            return 2;
        }
        txo.index = l3->l3_id;
        txo.scope = IP_TX_BOUND_L3;
        txop = &txo;
    }

    uint16_t id = (uint16_t)(get_current_proc_pid() & 0xFFFF);
    uint16_t seq0 = (uint16_t)(get_time() & 0xFFFF);
    uint32_t dead_streak = 0;

    for (uint32_t ttl = 1; ttl <= o->max_ttl; ttl++) {
        len = string_format_buf(line, sizeof(line), "%2u  ", ttl);
        uint32_t hop_ip = 0;
        bool any = false;

        for (uint32_t p = 0; p < o->count && len < sizeof(line); p++) {
            uint16_t seq = (uint16_t)(seq0 + (ttl << 6) + p);
            ping_result_t r = (ping_result_t){0};
            bool ok = icmp_ping(dst, id, seq, o->timeout_ms, txop, ttl, &r);
            if (r.responder_ip && hop_ip == 0) hop_ip = r.responder_ip;

            if (ok || r.status == PING_TTL_EXPIRED || r.status == PING_REDIRECT || r.status == PING_PARAM_PROBLEM ||
                r.status == PING_NET_UNREACH || r.status == PING_HOST_UNREACH || r.status == PING_ADMIN_PROHIBITED ||
                r.status == PING_FRAG_NEEDED || r.status == PING_SRC_ROUTE_FAILED) {
                any = true;
                len += string_format_buf(line + len, sizeof(line) - len, "%ums  ", r.rtt_ms);
            } else {
                len += string_format_buf(line + len, sizeof(line) - len, "*  ");
            }

            if (p + 1 < o->count) msleep(o->interval_ms);
        }

        if (any) {
            dead_streak = 0;
            if (hop_ip) {
                char hip[16];
                ipv4_to_string(hop_ip, hip);
                string_format_buf(line + len, sizeof(line) - len, "%s", hip);
            } else {
                string_format_buf(line + len, sizeof(line) - len, "???");
            }
        } else {
            dead_streak++;
            string_format_buf(line + len, sizeof(line) - len, "Request timed out.");
        }
        print("%s\n", line);

        if (hop_ip == dst) break;
        if (dead_streak >= o->timeout_streak_limit) {
            print("stopping after %u consecutive timeout hops\n", dead_streak);
            break;
        }
    }

    return 0;
}

static int tracert_v6(const tr_opts_t *o) {
    uint8_t dst[16] = {0};
    bool lit = ipv6_parse(o->host, dst);
    if (!lit) {
        dns_result_t dr = dns_resolve_aaaa(o->host, dst, DNS_USE_BOTH, o->timeout_ms);
        if (dr != DNS_OK) {
            print("tracert: dns lookup failed (%d) for '%s'\n", (int)dr, o->host);
            return 2;
        }
    }

    char dip[64];
    char line[256];
    ipv6_to_string(dst, dip, (int)sizeof(dip));
    print("Tracing route to %s [%s]\n", o->host, dip);
    size_t len = string_format_buf(line, sizeof(line), "hop  ");
    for (uint32_t p = 0; p < o->count && len < sizeof(line); p++) len += string_format_buf(line + len, sizeof(line) - len, "rtt%u  ", p + 1);
    string_format_buf(line + len, sizeof(line) - len, "address");
    print("%s\n", line);

    uint16_t id = (uint16_t)(get_current_proc_pid() & 0xFFFF);
    uint16_t seq0 = (uint16_t)(get_time() & 0xFFFF);
    uint32_t dead_streak = 0;

    for (uint32_t hl = 1; hl <= o->max_ttl; hl++) {
        len = string_format_buf(line, sizeof(line), "%2u  ", hl);
        uint8_t hop_ip[16] = {0};
        bool any = false;

        for (uint32_t p = 0; p < o->count && len < sizeof(line); p++) {
            uint16_t seq = (uint16_t)(seq0 + (hl << 6) + p);
            ping6_result_t r = (ping6_result_t){0};
            bool ok = icmpv6_ping(dst, id, seq, o->timeout_ms, NULL, (uint8_t)hl, &r);

            if (!ipv6_is_unspecified(r.responder_ip) && ipv6_is_unspecified(hop_ip)) ipv6_cpy(hop_ip, r.responder_ip);

            if (ok || r.status == PING_TTL_EXPIRED || r.status == PING_REDIRECT || r.status == PING_PARAM_PROBLEM ||
                r.status == PING_NET_UNREACH || r.status == PING_HOST_UNREACH || r.status == PING_ADMIN_PROHIBITED ||
                r.status == PING_FRAG_NEEDED || r.status == PING_SRC_ROUTE_FAILED) {
                any = true;
                len += string_format_buf(line + len, sizeof(line) - len, "%ums  ", r.rtt_ms);
            } else {
                len += string_format_buf(line + len, sizeof(line) - len, "*  ");
            }

            if (p + 1 < o->count) msleep(o->interval_ms);
        }

        if (any) {
            dead_streak = 0;
            if (!ipv6_is_unspecified(hop_ip)) {
                char hip[64];
                ipv6_to_string(hop_ip, hip, (int)sizeof(hip));
                string_format_buf(line + len, sizeof(line) - len, "%s", hip);
            } else {
                string_format_buf(line + len, sizeof(line) - len, "???");
            }
        } else {
            dead_streak++;
            string_format_buf(line + len, sizeof(line) - len, "Request timed out.");
        }
        print("%s\n", line);

        if (ipv6_cmp(hop_ip, dst) == 0) break;
        if (dead_streak >= o->timeout_streak_limit) {
            print("stopping after %u consecutive timeout hops\n", dead_streak);
            break;
        }
    }

    return 0;
}

int run_tracert(int argc, char *argv[]) {
    tr_opts_t o;
    if (!parse_args(argc, argv, &o)) {
        print("usage: tracert [-4/-6] [-m max_hops] [-n probes] [-w timeout_ms] [-i interval_ms] [-x stop_after_timeouts] [-s src_local_ip] host\n");
        return 2;
    }

    if (o.ver == IP_VER6) return tracert_v6(&o);
    return tracert_v4(&o);
}
