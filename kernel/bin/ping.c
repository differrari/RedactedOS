#include "ping.h"
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
    uint32_t count;
    uint32_t timeout_ms;
    uint32_t interval_ms;
    uint32_t ttl;
    uint32_t src_ip;
    bool src_set;
    const char *host;
} ping_opts_t;

static void help(file *fd) {
    const char *a = "usage: ping [-4/-6] [-n times] [-w timeout] [-i interval] [-t TTL] [-s src_local_ip] host";
    write_file(fd, a, strlen_max(a, STRING_MAX_LEN));
    write_file(fd, "\n", 1);
}

static bool parse_args(int argc, char *argv[], ping_opts_t *o) {
    o->ver = IP_VER4;
    o->count = 4;
    o->timeout_ms = 1000;
    o->interval_ms = 1000;
    o->ttl = 64;
    o->src_ip = 0;
    o->src_set = false;
    o->host = NULL;

    for (int i = 0; i < argc; ++i) {
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
                if (++i >= argc) return false;
                uint32_t src = 0;
                if (!ipv4_parse(argv[i], &src)) return false;
                o->src_ip = src;
                o->src_set = true;
            }
            else return false;
        } else {
            if (o->host) return false;
            o->host = a;
        }
    }

    if (!o->host) return false;
    return true;
}

static const char *status_to_msg(uint8_t st) {
    switch (st) {
    case PING_TIMEOUT: return "Request timed out.";
    case PING_NET_UNREACH: return "Destination Net Unreachable.";
    case PING_HOST_UNREACH: return "Destination Host Unreachable.";
    case PING_PROTO_UNREACH: return "Protocol Unreachable.";
    case PING_PORT_UNREACH: return "Port Unreachable.";
    case PING_FRAG_NEEDED: return "Fragmentation Needed.";
    case PING_SRC_ROUTE_FAILED: return "Source Route Failed.";
    case PING_ADMIN_PROHIBITED: return "Administratively Prohibited.";
    case PING_TTL_EXPIRED: return "Time To Live exceeded.";
    case PING_PARAM_PROBLEM: return "Parameter Problem.";
    case PING_REDIRECT: return "Redirect received.";
    default: return "No reply (unknown error).";
    }
}

static int ping_v4(file *fd, const ping_opts_t *o) {
    const char *host = o->host;

    uint32_t dst_ip_be = 0;
    bool is_lit = ipv4_parse(host, &dst_ip_be);
    if (!is_lit) {
        uint32_t r = 0;
        dns_result_t dr = dns_resolve_a(host, &r, DNS_USE_BOTH, o->timeout_ms);
        if (dr != DNS_OK) {
            string m = string_format("ping: dns lookup failed (%d) for '%s'", (int)dr, host);
            write_file(fd, m.data, m.length);
            write_file(fd, "\n", 1);
            free_sized(m.data, m.mem_length);
            return 2;
        }
        dst_ip_be = r;
    }

    char ipstr[16];
    ipv4_to_string(dst_ip_be, ipstr);

    write_file(fd, "PING ", 5);
    write_file(fd, host, strlen_max(host, STRING_MAX_LEN));
    write_file(fd, " (", 2);
    write_file(fd, ipstr, strlen_max(ipstr, STRING_MAX_LEN));
    write_file(fd, ") with 32 bytes of data:\n", 26);

    uint32_t sent = 0, received = 0, min_ms = UINT32_MAX, max_ms = 0;
    uint64_t sum_ms = 0;
    uint16_t id = (uint16_t)(get_current_proc_pid() & 0xFFFF);
    uint16_t seq_base = (uint16_t)(get_time() & 0xFFFF);

    ipv4_tx_opts_t txo = {0};
    const ipv4_tx_opts_t *txop = NULL;
    if (o->src_set) {
        l3_ipv4_interface_t *l3 = l3_ipv4_find_by_ip(o->src_ip);
        if (!l3) {
            const char *em = "ping: invalid source (no local ip match)\n";
            write_file(fd, em, strlen_max(em, STRING_MAX_LEN));
            return 2;
        }
        txo.index = (uint8_t)l3->l3_id;
        txo.scope = IP_TX_BOUND_L3;
        txop = &txo;
    }

    for (uint32_t i = 0; i < o->count; i++) {
        ++sent;
        uint16_t seq = (uint16_t)(seq_base + i);

        ping_result_t res = {0};
        bool ok = icmp_ping(dst_ip_be, id, seq, o->timeout_ms, txop, (uint8_t)o->ttl, &res);

        if (ok) {
            ++received;
            uint32_t rtt = res.rtt_ms;
            if (rtt < min_ms) min_ms = rtt;
            if (rtt > max_ms) max_ms = rtt;
            sum_ms += rtt;
            string ln = string_format("Reply from %s: bytes=32 time=%ums", ipstr, (uint32_t)rtt);
            write_file(fd, ln.data, ln.length);
            write_file(fd, "\n", 1);
            free_sized(ln.data, ln.mem_length);
        } else {
            const char *msg = status_to_msg(res.status);
            write_file(fd, msg, strlen(msg));
            write_file(fd, "\n", 1);
        }

        if (i + 1 < o->count) msleep(o->interval_ms);
    }

    write_file(fd, "\n", 1);

    string h = string_format("--- %s ping statistics ---", host);
    write_file(fd, h.data, h.length);
    write_file(fd, "\n", 1);
    free_sized(h.data, h.mem_length);

    uint32_t loss = (sent == 0) ? 0 : (uint32_t)((((uint64_t)(sent - received)) * 100) / sent);
    uint32_t total_time = (o->count > 0) ? (o->count - 1) * o->interval_ms : 0;

    string s = string_format("%u packets transmitted, %u received, %u%% packet loss, time %ums", sent, received, loss, total_time);
    write_file(fd, s.data, s.length);
    write_file(fd, "\n", 1);
    free_sized(s.data, s.mem_length);

    if (received > 0) {
        uint32_t avg = (uint32_t)(sum_ms / received);
        if (min_ms == UINT32_MAX) min_ms = avg;
        string r = string_format("rtt min/avg/max = %u/%u/%u ms", min_ms, avg, max_ms);
        write_file(fd, r.data, r.length);
        write_file(fd, "\n", 1);
        free_sized(r.data, r.mem_length);
    }

    return (received > 0) ? 0 : 1;
}

static int ping_v6(file *fd, const ping_opts_t *o) {
    const char *host = o->host;

    uint8_t dst6[16] ={0};
    bool is_lit = ipv6_parse(host, dst6);
    if (!is_lit) {
        dns_result_t dr = dns_resolve_aaaa(host, dst6, DNS_USE_BOTH, o->timeout_ms);
        if (dr != DNS_OK) {
            string m = string_format("ping: dns lookup failed (%d) for '%s'",(int)dr, host);
            write_file(fd, m.data, m.length);
            write_file(fd, "\n", 1);
            free_sized(m.data, m.mem_length);
            return 2;
        }
    }

    char ipstr[64];
    ipv6_to_string(dst6, ipstr, (int)sizeof(ipstr));

    write_file(fd, "PING ", 5);
    write_file(fd, host, strlen(host));
    write_file(fd, " (", 2);
    write_file(fd, ipstr, strlen(ipstr));
    write_file(fd, ") with 32 bytes of data:", 25);
    write_file(fd, "\n", 1);

    uint32_t sent = 0, received = 0, min_ms = UINT32_MAX, max_ms = 0;
    uint64_t sum_ms = 0;
    uint16_t id = (uint16_t)(get_current_proc_pid() & 0xFFFF);
    uint16_t seq_base = (uint16_t)(get_time() & 0xFFFF);

    for (uint32_t i = 0; i < o->count; i++) {
        ++sent;
        uint16_t seq = (uint16_t)(seq_base + i);

        ping6_result_t res = {0};
        bool ok = icmpv6_ping(dst6, id, seq, o->timeout_ms, NULL, (uint8_t)o->ttl, &res);

        if (ok) {
            ++received;
            uint32_t rtt = res.rtt_ms;
            if (rtt < min_ms) min_ms = rtt;
            if (rtt > max_ms) max_ms = rtt;
            sum_ms += rtt;

            string ln = string_format("Reply from %s: bytes=32 time=%ums", ipstr, (uint32_t)rtt);
            write_file(fd, ln.data, ln.length);
            write_file(fd, "\n", 1);
            free_sized(ln.data, ln.mem_length);
        } else {
            const char *msg = status_to_msg(res.status);
            write_file(fd, msg, strlen(msg));
            write_file(fd, "\n", 1);
        }

        if (i + 1 < o->count) msleep(o->interval_ms);
    }

    write_file(fd, "\n", 1);

    string h = string_format("--- %s ping statistics ---", host);
    write_file(fd, h.data, h.length);
    write_file(fd, "\n", 1);
    free_sized(h.data, h.mem_length);


    uint32_t loss = (sent == 0) ? 0 : (uint32_t)((((uint64_t)(sent - received)) * 100) / sent);
    uint32_t total_time = (o->count > 0) ? (o->count - 1) * o->interval_ms : 0;

    string s = string_format("%u packets transmitted, %u received, %u%% packet loss, time %ums", sent, received, loss, total_time);
    write_file(fd, s.data, s.length);
    write_file(fd, "\n", 1);
    free_sized(s.data, s.mem_length);

    if (received > 0) {
        uint32_t avg = (uint32_t)(sum_ms / received);
        if (min_ms == UINT32_MAX) min_ms = avg;
        string r = string_format("rtt min/avg/max = %u/%u/%u ms", min_ms, avg, max_ms);
        write_file(fd, r.data, r.length);
        write_file(fd, "\n", 1);
        free_sized(r.data, r.mem_length);
    }

    return (received > 0) ? 0 : 1;
}

int run_ping(int argc, char *argv[]) {
    uint16_t pid = get_current_proc_pid();
    string p = string_format("/proc/%u/out", pid);
    file fd = {0};
    open_file(p.data, &fd);
    free_sized(p.data, p.mem_length);

    ping_opts_t opts;
    if (!parse_args(argc, argv, &opts)) {
        help(&fd);
        close_file(&fd);
        return 2;
    }

    if (opts.ver == IP_VER6 && opts.src_set) {
        const char *em = "ping: -s is only supported for IPv4\n";
        write_file(&fd, em, strlen(em));
        close_file(&fd);
        return 2;
    }

    if (opts.ver == IP_VER4 && opts.src_set) {
        l3_ipv4_interface_t *l3 = l3_ipv4_find_by_ip(opts.src_ip);
        if (!l3) {
            char ssrc[16];
            ipv4_to_string(opts.src_ip, ssrc);
            string em = string_format("ping: invalid source %s (no local ip match)", ssrc);
            write_file(&fd, em.data, em.length);
            write_file(&fd, "\n", 1);
            free_sized(em.data, em.mem_length);
            close_file(&fd);
            return 2;
        }
    }

    int rc = 0;
    if (opts.ver == IP_VER4) rc = ping_v4(&fd, &opts);
    else if (opts.ver == IP_VER6) rc = ping_v6(&fd, &opts);
    else { help(&fd); rc = 2; }

    close_file(&fd);
    return rc;
}
