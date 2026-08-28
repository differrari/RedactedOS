#include "tracert.h"
#include "icmp_probe.h"
#include "std/string.h"
#include "std/memory.h"
#include "types.h"
#include "console/kio.h"
#include "process/scheduler.h"
#include "syscalls/syscalls.h"
#include "networking/application_layer/dns/dns.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/transport_layer/trans_utils.h"

typedef struct {
    ip_version_t ver;
    uint32_t max_ttl;
    uint32_t count;
    uint32_t timeout_ms;
    uint32_t interval_ms;
    uint32_t timeout_streak_limit;
    SockBindSpec bind;
    bool bind_set;
    const char *host;
} tr_opts_t;

static void print_help(void) {
    print("Usage:\t tracert [OPTION] HOST");
    print("trace route to HOST\n");
    print("Args:");
    print("\t HOST\t host|ip");
    print("\t SOURCE\t IP|l2:ID|l3:ID\n");
    print("Options:");
    print("\t -4\t ipv4");
    print("\t -6\t ipv6");
    print("\t -m MAX_HOPS\t max hops (30)");
    print("\t -n PROBES\t probes per hop (3)");
    print("\t -w TIMEOUT\t timeout ms (1000)");
    print("\t -i INTERVAL\t interval ms (250)");
    print("\t -x STREAK\t timeout hops (5)");
    print("\t -s SOURCE\t source");
    print("\t --help\t help");
}

static bool parse_args(int argc, char *argv[], tr_opts_t *o) {
    o->ver = IP_VER4;
    o->max_ttl = 30;
    o->count = 3;
    o->timeout_ms = 1000;
    o->interval_ms = 250;
    o->timeout_streak_limit = 5;
    o->bind_set = false;
    o->host = NULL;
    memset(&o->bind, 0, sizeof(o->bind));

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a && a[0] == '-') {
            if (strcmp_case(a, "-4",true) == 0) o->ver = IP_VER4;
            else if (strcmp_case(a, "-6",true) == 0) o->ver = IP_VER6;
            else if (strcmp_case(a, "-m",true) == 0) {
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
                if (++i >= argc || o->bind_set) return false;
                if (!icmp_probe_parse_bind(argv[i], &o->bind)) return false;
                o->bind_set = true;
            } else return false;
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
    uint32_t dst_ip = 0;
    bool lit = ipv4_parse(o->host, &dst_ip);
    if (!lit) {
        uint32_t r = 0;
        dns_result_t dr = dns_resolve_a(o->host, &r, DNS_USE_BOTH, o->timeout_ms);
        if (dr != DNS_OK) {
            print("tracert: dns lookup failed (%d) for '%s'", (int)dr, o->host);
            return 2;
        }
        dst_ip = r;
    }

    char dip[16];
    char line[256];
    ipv4_to_string(dst_ip, dip);
    print("Tracing route to %s [%s]", o->host, dip);
    size_t len = string_format_buf(line, sizeof(line), "hop  ");
    for (uint32_t p = 0; p < o->count && len < sizeof(line); p++) len += string_format_buf(line + len, sizeof(line) - len, "rtt%u  ", p + 1);
    string_format_buf(line + len, sizeof(line) - len, "address");
    print("%s", line);

    uint16_t id = (uint16_t)(get_current_proc_pid() & 0xFFFF);
    uint16_t seq0 = (uint16_t)(get_time() & 0xFFFF);
    uint32_t dead_streak = 0;
    const SockBindSpec* bind = o->bind_set ? &o->bind : NULL;
    net_l4_endpoint dst;
    make_ep(&dst_ip, 0, IP_VER4, &dst);

    for (uint32_t ttl = 1; ttl <= o->max_ttl; ttl++) {
        len = string_format_buf(line, sizeof(line), "%2u  ", ttl);
        uint32_t hop_ip = 0;
        bool any = false;
        bool reached = false;

        for (uint32_t p = 0; p < o->count && len < sizeof(line); p++) {
            uint16_t seq = (uint16_t)(seq0 + (ttl << 6) + p);
            icmp_probe_result_t r;
            memset(&r, 0, sizeof(r));
            bool answered = icmp_probe_collect(&dst, id, seq, o->timeout_ms, bind, (uint8_t)ttl, &r, 1) && r.status != ICMP_PROBE_UNKNOWN_ERROR;
            if (r.responder.ver == IP_VER4 && hop_ip == 0) memcpy(&hop_ip, r.responder.ip, sizeof(hop_ip));

            if (answered) {
                any = true;
                reached |= r.status == ICMP_PROBE_OK;
                len += string_format_buf(line + len, sizeof(line) - len, "%ums  ", r.rtt_ms);
            } else len += string_format_buf(line + len, sizeof(line) - len, "*  ");

            if (p + 1 < o->count) msleep(o->interval_ms);
        }

        if (any) {
            dead_streak = 0;
            if (hop_ip) {
                char hip[16];
                ipv4_to_string(hop_ip, hip);
                string_format_buf(line + len, sizeof(line) - len, "%s", hip);
            } else string_format_buf(line + len, sizeof(line) - len, "???");
        } else {
            dead_streak++;
            string_format_buf(line + len, sizeof(line) - len, "Request timed out.");
        }
        print("%s", line);

        if (reached || hop_ip == dst_ip) break;
        if (dead_streak >= o->timeout_streak_limit) {
            print("stopping after %u consecutive timeout hops", dead_streak);
            break;
        }
    }

    return 0;
}

static int tracert_v6(const tr_opts_t *o) {
    uint8_t dst_ip[16] = {0};
    bool lit = ipv6_parse(o->host, dst_ip);
    if (!lit) {
        dns_result_t dr = dns_resolve_aaaa(o->host, dst_ip, DNS_USE_BOTH, o->timeout_ms);
        if (dr != DNS_OK) {
            print("tracert: dns lookup failed (%d) for '%s'", (int)dr, o->host);
            return 2;
        }
    }

    if (ipv6_is_linkscope_mcast(dst_ip) && !o->bind_set) {
        print("tracert: IPv6 link-local multicast requires -s ipv6, l2:id or l3:id");
        return 2;
    }

    char dip[64];
    char line[256];
    ipv6_to_string(dst_ip, dip, (int)sizeof(dip));
    print("Tracing route to %s [%s]", o->host, dip);
    size_t len = string_format_buf(line, sizeof(line), "hop  ");
    for (uint32_t p = 0; p < o->count && len < sizeof(line); p++) len += string_format_buf(line + len, sizeof(line) - len, "rtt%u  ", p + 1);
    string_format_buf(line + len, sizeof(line) - len, "address");
    print("%s", line);

    uint16_t id = (uint16_t)(get_current_proc_pid() & 0xFFFF);
    uint16_t seq0 = (uint16_t)(get_time() & 0xFFFF);
    uint32_t dead_streak = 0;
    const SockBindSpec* bind = o->bind_set ? &o->bind : NULL;
    net_l4_endpoint dst;
    make_ep(dst_ip, 0, IP_VER6, &dst);

    for (uint32_t hl = 1; hl <= o->max_ttl; hl++) {
        len = string_format_buf(line, sizeof(line), "%2u  ", hl);
        uint8_t hop_ip[16] = {0};
        bool any = false;
        bool reached = false;

        for (uint32_t p = 0; p < o->count && len < sizeof(line); p++) {
            uint16_t seq = (uint16_t)(seq0 + (hl << 6) + p);
            icmp_probe_result_t r;
            memset(&r, 0, sizeof(r));
            bool answered = icmp_probe_collect(&dst, id, seq, o->timeout_ms, bind, (uint8_t)hl, &r, 1) && r.status != ICMP_PROBE_UNKNOWN_ERROR;
            if (r.responder.ver == IP_VER6 && ipv6_is_unspecified(hop_ip)) ipv6_cpy(hop_ip, r.responder.ip);

            if (answered) {
                any = true;
                reached |= r.status == ICMP_PROBE_OK;
                len += string_format_buf(line + len, sizeof(line) - len, "%ums  ", r.rtt_ms);
            } else len += string_format_buf(line + len, sizeof(line) - len, "*  ");

            if (p + 1 < o->count) msleep(o->interval_ms);
        }

        if (any) {
            dead_streak = 0;
            if (!ipv6_is_unspecified(hop_ip)) {
                char hip[64];
                ipv6_to_string(hop_ip, hip, (int)sizeof(hip));
                string_format_buf(line + len, sizeof(line) - len, "%s", hip);
            } else string_format_buf(line + len, sizeof(line) - len, "???");
        } else {
            dead_streak++;
            string_format_buf(line + len, sizeof(line) - len, "Request timed out.");
        }
        print("%s", line);

        if (reached || ipv6_cmp(hop_ip, dst_ip) == 0) break;
        if (dead_streak >= o->timeout_streak_limit) {
            print("stopping after %u consecutive timeout hops", dead_streak);
            break;
        }
    }

    return 0;
}

int run_tracert(int argc, char *argv[]) {
    if (argc == 2 && strcmp_case(argv[1], "--help", true) == 0) {
        print_help();
        return 0;
    }

    tr_opts_t o;
    if (!parse_args(argc, argv, &o)) {
        print_help();
        return 2;
    }

    if (o.bind_set && o.bind.kind == BIND_IP && o.bind.ver != o.ver) {
        print("tracert: source address version doesn't' match target version");
        return 2;
    }

    if (o.ver == IP_VER6) return tracert_v6(&o);
    return tracert_v4(&o);
}
