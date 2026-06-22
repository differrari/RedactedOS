#include "ping.h"
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

#define PING_MAX_REPLIES 16

typedef struct {
    const char *host;
    SockBindSpec bind;
    bool bind_set;
} ping_addressing_t;

typedef struct {
    ip_version_t ver;
    uint32_t count;
    uint32_t timeout_ms;
    uint32_t interval_ms;
    uint32_t ttl;
    ping_addressing_t addr;
} ping_opts_t;

static bool parse_args(int argc, char *argv[], ping_opts_t *o) {
    o->ver = IP_VER4;
    o->count = 4;
    o->timeout_ms = 1000;
    o->interval_ms = 1000;
    o->ttl = 64;
    memset(&o->addr, 0, sizeof(o->addr));

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (a && a[0] == '-') {
            if (strcmp_case(a, "-4",true) == 0) o->ver = IP_VER4;
            else if (strcmp_case(a, "-6",true) == 0) o->ver = IP_VER6;
            else if (strcmp_case(a, "-n",true) == 0) {
                if (++i >= argc) return false;
                if (!parse_uint32_dec(argv[i], &o->count) || o->count == 0) return false;
            } else if (strcmp_case(a, "-w",true) == 0) {
                if (++i >= argc) return false;
                if (!parse_uint32_dec(argv[i], &o->timeout_ms) || o->timeout_ms == 0) return false;
            } else if (strcmp_case(a, "-i",true) == 0) {
                if (++i >= argc) return false;
                if (!parse_uint32_dec(argv[i], &o->interval_ms)) return false;
            } else if (strcmp_case(a, "-t",true) == 0) {
                if (++i >= argc) return false;
                if (!parse_uint32_dec(argv[i], &o->ttl)) return false;
            } else if (strcmp_case(a, "-s",true) == 0) {
                if (++i >= argc || o->addr.bind_set) return false;
                if (!icmp_probe_parse_bind(argv[i], &o->addr.bind)) return false;
                o->addr.bind_set = true;
            } else return false;
        } else {
            if (o->addr.host) return false;
            o->addr.host = a;
        }
    }

    if (!o->addr.host) return false;
    return true;
}

static const char *status_to_msg(uint8_t st) {
    switch (st) {
        case ICMP_PROBE_TIMEOUT: return "Request timed out.";
        case ICMP_PROBE_NET_UNREACH: return "Destination Net Unreachable.";
        case ICMP_PROBE_HOST_UNREACH: return "Destination Host Unreachable.";
        case ICMP_PROBE_PROTO_UNREACH: return "Protocol Unreachable.";
        case ICMP_PROBE_PORT_UNREACH: return "Port Unreachable.";
        case ICMP_PROBE_FRAG_NEEDED: return "Fragmentation Needed.";
        case ICMP_PROBE_SRC_ROUTE_FAILED: return "Source Route Failed.";
        case ICMP_PROBE_ADMIN_PROHIBITED: return "Administratively Prohibited.";
        case ICMP_PROBE_TTL_EXPIRED: return "Time To Live exceeded.";
        case ICMP_PROBE_PARAM_PROBLEM: return "Parameter Problem.";
        case ICMP_PROBE_REDIRECT: return "Redirect received.";
        default: return "No reply (unknown error).";
    }
}

static int ping_v4(const ping_opts_t *o) {
    const char *host = o->addr.host;

    uint32_t dst_ip = 0;
    bool is_lit = ipv4_parse(host, &dst_ip);
    if (!is_lit) {
        uint32_t r = 0;
        dns_result_t dr = dns_resolve_a(host, &r, DNS_USE_BOTH, o->timeout_ms);
        if (dr != DNS_OK) {
            print("ping: dns lookup failed (%d) for '%s'", (int)dr, host);
            return 2;
        }
        dst_ip = r;
    }

    if (ipv4_is_limited_broadcast(dst_ip) && (!o->addr.bind_set || o->addr.bind.kind == BIND_L2)) {
        print("ping: limited broadcast requires -s local_ipv4 or l3:id");
        return 2;
    }

    char ipstr[16];
    ipv4_to_string(dst_ip, ipstr);
    print("PING %s (%s) with 32 bytes of data:", host, ipstr);

    uint32_t sent = 0, received = 0, min_ms = UINT32_MAX, max_ms = 0;
    uint64_t sum_ms = 0;
    uint16_t id = (uint16_t)(get_current_proc_pid() & 0xFFFF);
    uint16_t seq_base = (uint16_t)(get_time() & 0xFFFF);
    const SockBindSpec* bind = o->addr.bind_set ? &o->addr.bind : NULL;
    bool multi = ipv4_is_multicast(dst_ip) || ipv4_is_limited_broadcast(dst_ip);
    uint32_t max_results = multi ? PING_MAX_REPLIES : 1;
    net_l4_endpoint dst;
    make_ep(&dst_ip, 0, IP_VER4, &dst);

    for (uint32_t i = 0; i < o->count; i++) {
        sent++;
        uint16_t seq = (uint16_t)(seq_base + i);
        icmp_probe_result_t res[PING_MAX_REPLIES];
        uint32_t n = icmp_probe_collect(&dst, id, seq, o->timeout_ms, bind, (uint8_t)o->ttl, res, max_results);

        if (n) {
            for (uint32_t j = 0; j < n; j++) {
                if (res[j].status == ICMP_PROBE_OK) {
                    received++;
                    uint32_t rtt = res[j].rtt_ms;
                    if (rtt < min_ms) min_ms = rtt;
                    if (rtt > max_ms) max_ms = rtt;
                    sum_ms += rtt;
                    char rip[64];
                    net_ep_split(&res[j].responder, rip, (int)sizeof(rip), NULL, NULL);
                    print("Reply from %s: bytes=32 time=%ums", rip, rtt);
                } else print("%s", status_to_msg(res[j].status));
            }
        } else print("%s", status_to_msg(ICMP_PROBE_TIMEOUT));

        if (i + 1 < o->count) msleep(o->interval_ms);
    }

    print("");
    print("--- %s ping statistics ---", host);

    uint32_t loss = (sent == 0 || received >= sent) ? 0 : (uint32_t)((((uint64_t)(sent - received)) * 100) / sent);
    uint32_t total_time = (o->count > 0) ? (o->count - 1) * o->interval_ms : 0;

    print("%u packets transmitted, %u received, %u%% packet loss, time %ums", sent, received, loss, total_time);

    if (received > 0) {
        uint32_t avg = (uint32_t)(sum_ms / received);
        if (min_ms == UINT32_MAX) min_ms = avg;
        print("rtt min/avg/max = %u/%u/%u ms", min_ms, avg, max_ms);
    }

    return received > 0 ? 0 : 1;
}

static int ping_v6(const ping_opts_t *o) {
    const char *host = o->addr.host;
    uint8_t dst_ip[16] ={0};
    bool is_lit = ipv6_parse(host, dst_ip);
    if (!is_lit) {
        dns_result_t dr = dns_resolve_aaaa(host, dst_ip, DNS_USE_BOTH, o->timeout_ms);
        if (dr != DNS_OK) {
            print("ping: dns lookup failed (%d) for '%s'",(int)dr, host);
            return 2;
        }
    }

    if (ipv6_is_linkscope_mcast(dst_ip) && !o->addr.bind_set) {
        print("ping: IPv6 link-local multicast requires -s local_ipv6, l2:id or l3:id");
        return 2;
    }

    char ipstr[64];
    ipv6_to_string(dst_ip, ipstr, (int)sizeof(ipstr));

    print("PING %s (%s) with 32 bytes of data:", host, ipstr);

    uint32_t sent = 0, received = 0, min_ms = UINT32_MAX, max_ms = 0;
    uint64_t sum_ms = 0;
    uint16_t id = (uint16_t)(get_current_proc_pid() & 0xFFFF);
    uint16_t seq_base = (uint16_t)(get_time() & 0xFFFF);
    const SockBindSpec* bind = o->addr.bind_set ? &o->addr.bind : NULL;
    uint32_t max_results = ipv6_is_multicast(dst_ip) ? PING_MAX_REPLIES : 1;
    net_l4_endpoint dst;
    make_ep(dst_ip, 0, IP_VER6, &dst);

    for (uint32_t i = 0; i < o->count; i++) {
        sent++;
        uint16_t seq = (uint16_t)(seq_base + i);
        icmp_probe_result_t res[PING_MAX_REPLIES];
        uint32_t n = icmp_probe_collect(&dst, id, seq, o->timeout_ms, bind, (uint8_t)o->ttl, res, max_results);

        if (n) {
            for (uint32_t j = 0; j < n; j++) {
                if (res[j].status == ICMP_PROBE_OK) {
                    received++;
                    uint32_t rtt = res[j].rtt_ms;
                    if (rtt < min_ms) min_ms = rtt;
                    if (rtt > max_ms) max_ms = rtt;
                    sum_ms += rtt;
                    char rip[64];
                    net_ep_split(&res[j].responder, rip, (int)sizeof(rip), NULL, NULL);
                    print("Reply from %s: bytes=32 time=%ums", rip, rtt);
                } else print("%s", status_to_msg(res[j].status));
            }
        } else print("%s", status_to_msg(ICMP_PROBE_TIMEOUT));

        if (i + 1 < o->count) msleep(o->interval_ms);
    }

    print("");

    print("--- %s ping statistics ---", host);

    uint32_t loss = (sent == 0 || received >= sent) ? 0 : (uint32_t)((((uint64_t)(sent - received)) * 100) / sent);
    uint32_t total_time = (o->count > 0) ? (o->count - 1) * o->interval_ms : 0;

    print("%u packets transmitted, %u received, %u%% packet loss, time %ums", sent, received, loss, total_time);

    if (received > 0) {
        uint32_t avg = (uint32_t)(sum_ms / received);
        if (min_ms == UINT32_MAX) min_ms = avg;
        print("rtt min/avg/max = %u/%u/%u ms", min_ms, avg, max_ms);
    }

    return (received > 0) ? 0 : 1;
}

int run_ping(int argc, char *argv[]) {
    ping_opts_t opts;
    if (!parse_args(argc, argv, &opts)) {
        print("usage: ping [-4/-6] [-n times] [-w timeout] [-i interval] [-t TTL] [-s ip|l2:id|l3:id] host");
        return 2;
    }

    if (opts.addr.bind_set && opts.addr.bind.kind == BIND_IP && opts.addr.bind.ver != opts.ver) {
        print("ping: source address version doesn't match target version");
        return 2;
    }

    if (opts.ver == IP_VER4) return ping_v4(&opts);
    if (opts.ver == IP_VER6) return ping_v6(&opts);
    print("usage: ping [-4/-6] [-n times] [-w timeout] [-i interval] [-t TTL] [-s ip|l2:id|l3:id] host");

    return 2;
}
