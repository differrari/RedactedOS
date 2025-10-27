#include "net/internet_layer/icmp.h"
#include "net/network_types.h"
#include "std/string.h"
#include "std/memory.h"
#include "types.h"
#include "console/kio.h"
#include "filesystem/filesystem.h"
#include "process/scheduler.h"
#include "syscalls/syscalls.h"
#include "net/internet_layer/ipv4.h"
#include "net/internet_layer/ipv4_route.h"
#include "net/application_layer/dns.h"
#include "net/internet_layer/ipv4_utils.h"

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

static void help(file *fd) {
    const char *a = "usage: tracert [-4/-6] [-m max_ttl] [-n probes] [-w timeout] [-i interval] [-x dead_hops] [-s src_local_ip] host";
    write_file(fd, a, strlen(a, STRING_MAX_LEN));
    write_file(fd, "\n", 1);
}

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

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (a && a[0] == '-') {
            if (strcmp(a, "-4", true) == 0) {
                o->ver = IP_VER4;
            } else if (strcmp(a, "-6", true) == 0) {
                o->ver = IP_VER6;
            } else if (strcmp(a, "-m", true) == 0) {
                if (++i >= argc) return false;
                if (!parse_uint32_dec(argv[i], &o->max_ttl) || o->max_ttl == 0) return false;
            } else if (strcmp(a, "-n", true) == 0) {
                if (++i >= argc) return false;
                if (!parse_uint32_dec(argv[i], &o->count) || o->count == 0) return false;
            } else if (strcmp(a, "-w", true) == 0) {
                if (++i >= argc) return false;
                if (!parse_uint32_dec(argv[i], &o->timeout_ms) || o->timeout_ms == 0) return false;
            } else if (strcmp(a, "-i", true) == 0) {
                if (++i >= argc) return false;
                if (!parse_uint32_dec(argv[i], &o->interval_ms)) return false;
            } else if (strcmp(a, "-x", true) == 0) {
                if (++i >= argc) return false;
                if (!parse_uint32_dec(argv[i], &o->timeout_streak_limit) || o->timeout_streak_limit == 0) return false;
            } else if (strcmp(a, "-s", true) == 0) {
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

static int tracert_v4(file *fd, const tr_opts_t *o) {
    uint32_t dst = 0;
    bool lit = ipv4_parse(o->host, &dst);
    if (!lit) {
        uint32_t r = 0;
        dns_result_t dr = dns_resolve_a(o->host, &r, DNS_USE_BOTH, o->timeout_ms);
        if (dr != DNS_OK) {
            string m = string_format("tracert: dns lookup failed (%d) for '%s'", (int)dr, o->host);
            write_file(fd, m.data, m.length);
            write_file(fd, "\n", 1);
            //write_file(fd, "\t", 1);
            free(m.data, m.mem_length);
            return 2;
        }
        dst = r;
    }

    char dip[16];
    ipv4_to_string(dst, dip);
    write_file(fd, "Tracing route to ", 17);
    write_file(fd, o->host, strlen(o->host, STRING_MAX_LEN));
    write_file(fd, " [", 2);
    write_file(fd, dip, strlen(dip, STRING_MAX_LEN));
    write_file(fd, "]", 1);
    write_file(fd, "\n", 1);

    write_file(fd, "hop  ", 5);
    for (uint32_t p = 0; p < o->count; p++) {
        string col = string_format("rtt%u  ", (uint32_t)(p + 1));
        write_file(fd, col.data, col.length);
        free(col.data, col.mem_length);
    }
    write_file(fd, "address", 7);
    write_file(fd, "\n", 1);

    ipv4_tx_opts_t txo = (ipv4_tx_opts_t){0};
    const ipv4_tx_opts_t *txop = NULL;
    if (o->src_set) {
        l3_ipv4_interface_t *l3 = l3_ipv4_find_by_ip(o->src_ip);
        if (!l3) {
            char ssrc[16];
            ipv4_to_string(o->src_ip, ssrc);
            string em = string_format("tracert: invalid source %s (no local ip match)", ssrc);
            write_file(fd, em.data, em.length);
            write_file(fd, "\n", 1);
            free(em.data, em.mem_length);
            return 2;
        }
        txo.index = l3->l3_id;
        txo.scope = IPV4_TX_BOUND_L3;
        txop = &txo;
    }

    uint16_t id = (uint16_t)(get_current_proc_pid() & 0xFFFF);
    uint16_t seq0 = (uint16_t)(get_time() & 0xFFFF);
    uint32_t dead_streak = 0;

    for (uint32_t ttl = 1; ttl <= o->max_ttl; ttl++) {
        string hdr = string_format("%2u  ", ttl);
        write_file(fd, hdr.data, hdr.length);
        free(hdr.data, hdr.mem_length);

        uint32_t hop_ip = 0;
        bool any = false;

        for (uint32_t p = 0; p < o->count; p++) {
            uint16_t seq = (uint16_t)(seq0 + (ttl << 6) + p);
            ping_result_t r = (ping_result_t){0};
            bool ok = icmp_ping(dst, id, seq, o->timeout_ms, txop, ttl, &r);
            if (r.responder_ip && hop_ip == 0) hop_ip = r.responder_ip;

            if (ok) {
                any = true;
                string ms = string_format("%ums  ", r.rtt_ms);
                write_file(fd, ms.data, ms.length);
                free(ms.data, ms.mem_length);
            } else {
                if (r.status == PING_TTL_EXPIRED || r.status == PING_REDIRECT || r.status == PING_PARAM_PROBLEM ||
                    r.status == PING_NET_UNREACH || r.status == PING_HOST_UNREACH || r.status == PING_ADMIN_PROHIBITED ||
                    r.status == PING_FRAG_NEEDED || r.status == PING_SRC_ROUTE_FAILED) {
                    any = true;
                    string ms = string_format("%ums  ", r.rtt_ms);
                    write_file(fd, ms.data, ms.length);
                    free(ms.data, ms.mem_length);
                } else {
                    write_file(fd, "*  ", 3);
                }
            }

            if (p + 1 < o->count) sleep(o->interval_ms);
        }

        if (any) {
            dead_streak = 0;
            if (hop_ip) {
                char hip[16];
                ipv4_to_string(hop_ip, hip);
                write_file(fd, hip, strlen(hip, STRING_MAX_LEN));
            } else {
                write_file(fd, "???", 3);
            }
        } else {
            dead_streak++;
            write_file(fd, "Request timed out.", 19);
        }
        write_file(fd, "\n", 1);

        if (hop_ip == dst) break;
        if (dead_streak >= o->timeout_streak_limit) {
            string note = string_format("stopping after %u consecutive timeout hops", dead_streak);
            write_file(fd, note.data, note.length);
            write_file(fd, "\n", 1);
            free(note.data, note.mem_length);
            break;
        }
    }

    return 0;
}

int run_tracert(int argc, char *argv[]) {
    uint16_t pid = get_current_proc_pid();
    string p = string_format("/proc/%u/out", pid);
    file fd = (file){0};
    open_file(p.data, &fd);
    free(p.data, p.mem_length);

    tr_opts_t o;
    if (!parse_args(argc, argv, &o)) {
        help(&fd);
        close_file(&fd);
        return 2;
    }

    if (o.ver == IP_VER6) {//unimplemented
        return 3;
    }

    int rc = tracert_v4(&fd, &o);
    close_file(&fd);
    return rc;
}
