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
#include "networking/interface_manager.h"
#include "networking/transport_layer/trans_utils.h"

#define PING_MAX_REPLIES 16

typedef struct {
    const char *host;
    net_l4_endpoint src_ip;
    ip_tx_opts_t src_if;
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
                if (++i >= argc) return false;
                if (o->addr.src_ip.ver || o->addr.src_if.scope != IP_TX_AUTO) return false;
                uint32_t id = 0;
                if (strncmp(argv[i], "l2:", 3) == 0) {
                    if (!parse_uint32_dec_exact(argv[i] + 3, &id) || id == 0 || id > UINT8_MAX) return false;
                    o->addr.src_if.index = (uint8_t)id;
                    o->addr.src_if.scope = IP_TX_BOUND_L2;
                } else if (strncmp(argv[i], "l3:", 3) == 0) {
                    if (!parse_uint32_dec_exact(argv[i] + 3, &id) || id == 0 || id > UINT8_MAX) return false;
                    o->addr.src_if.index = (uint8_t)id;
                    o->addr.src_if.scope = IP_TX_BOUND_L3;
                } else {
                    uint32_t src4 = 0;
                    uint8_t src6[16];
                    if (ipv4_parse(argv[i], &src4)) make_ep(&src4, 0, IP_VER4, &o->addr.src_ip);
                    else if (ipv6_parse(argv[i], src6)) make_ep(src6, 0, IP_VER6, &o->addr.src_ip);
                    else return false;
                }
            }
            else return false;
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

static int ping_v4(const ping_opts_t *o) {
    const char *host = o->addr.host;

    uint32_t dst_ip_be = 0;
    bool is_lit = ipv4_parse(host, &dst_ip_be);
    if (!is_lit) {
        uint32_t r = 0;
        dns_result_t dr = dns_resolve_a(host, &r, DNS_USE_BOTH, o->timeout_ms);
        if (dr != DNS_OK) {
            print("ping: dns lookup failed (%d) for '%s'", (int)dr, host);
            return 2;
        }
        dst_ip_be = r;
    }

    char ipstr[16];
    ipv4_to_string(dst_ip_be, ipstr);

    print("PING %s (%s) with 32 bytes of data:", host, ipstr);

    uint32_t sent = 0, received = 0, min_ms = UINT32_MAX, max_ms = 0;
    uint64_t sum_ms = 0;
    uint16_t id = (uint16_t)(get_current_proc_pid() & 0xFFFF);
    uint16_t seq_base = (uint16_t)(get_time() & 0xFFFF);

    ip_tx_opts_t txo = {0};
    const ip_tx_opts_t *txop = NULL;
    if (o->addr.src_ip.ver) {
        uint32_t src4 = 0;
        memcpy(&src4, o->addr.src_ip.ip, 4);
        l3_ipv4_interface_t *l3 = l3_ipv4_find_by_ip(src4);
        txo.index = l3->l3_id;
        txo.scope = IP_TX_BOUND_L3;
        txop = &txo;
    } else if (o->addr.src_if.scope != IP_TX_AUTO) txop = &o->addr.src_if;
    else if (ipv4_is_limited_broadcast(dst_ip_be)) {
        l3_ipv4_interface_t *chosen = NULL;
        uint8_t cnt = l2_interface_count();
        for (uint8_t li = 0; li < cnt; li++) {
            l2_interface_t *l2 = l2_interface_at(li);
            if (!l2 || !l2->is_up) continue;
            for (int vi = 0; vi < MAX_IPV4_PER_INTERFACE; vi++) {
                l3_ipv4_interface_t *v4 = l2->l3_v4[vi];
                if (!ipv4_l3_is_ready(v4) || v4->is_localhost) continue;
                if (chosen) {
                    print("ping: broadcast requires -s when more IPv4 interfaces are usable");
                    return 2;
                }
                chosen = v4;
            }
        }
        if (!chosen) {
            print("ping: no usable IPv4 interface");
            return 2;
        }
        txo.index = (uint8_t)chosen->l3_id;
        txo.scope = IP_TX_BOUND_L3;
        txop = &txo;
    }

    bool multi = ipv4_is_multicast(dst_ip_be) || ipv4_is_limited_broadcast(dst_ip_be);
    uint32_t max_results = multi ? PING_MAX_REPLIES : 1;
    for (uint32_t i = 0; i < o->count; i++) {
        ++sent;
        uint16_t seq = (uint16_t)(seq_base + i);

        ping_result_t res[PING_MAX_REPLIES];
        uint32_t n = icmp_ping_collect(dst_ip_be, id, seq, o->timeout_ms, txop, (uint8_t)o->ttl, res, max_results);

        if (n) {
            for (uint32_t j = 0; j < n; j++) {
                if (res[j].status == PING_OK) {
                    received++;
                    uint32_t rtt = res[j].rtt_ms;
                    if (rtt < min_ms) min_ms = rtt;
                    if (rtt > max_ms) max_ms = rtt;
                    sum_ms += rtt;
                    char rip[16];
                    ipv4_to_string(res[j].responder_ip, rip);
                    print("Reply from %s: bytes=32 time=%ums", rip, rtt);
                } else {
                    print("%s", status_to_msg(res[j].status));
                }
            }
        } else {
            print("%s", status_to_msg(PING_TIMEOUT));
        }

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

static int ping_v6(const ping_opts_t *o) {
    const char *host = o->addr.host;

    uint8_t dst6[16] ={0};
    bool is_lit = ipv6_parse(host, dst6);
    if (!is_lit) {
        dns_result_t dr = dns_resolve_aaaa(host, dst6, DNS_USE_BOTH, o->timeout_ms);
        if (dr != DNS_OK) {
            print("ping: dns lookup failed (%d) for '%s'",(int)dr, host);
            return 2;
        }
    }

    char ipstr[64];
    ipv6_to_string(dst6, ipstr, (int)sizeof(ipstr));

    print("PING %s (%s) with 32 bytes of data:", host, ipstr);

    uint32_t sent = 0, received = 0, min_ms = UINT32_MAX, max_ms = 0;
    uint64_t sum_ms = 0;
    uint16_t id = (uint16_t)(get_current_proc_pid() & 0xFFFF);
    uint16_t seq_base = (uint16_t)(get_time() & 0xFFFF);

    ip_tx_opts_t txo = {0};
    const ip_tx_opts_t *txop = NULL;
    if (o->addr.src_ip.ver) {
        l3_ipv6_interface_t *l3 = l3_ipv6_find_by_ip(o->addr.src_ip.ip);
        txo.index = l3->l3_id;
        txo.scope = IP_TX_BOUND_L3;
        txop = &txo;
    } else if (o->addr.src_if.scope != IP_TX_AUTO) txop = &o->addr.src_if;
    else if (ipv6_is_linkscope_mcast(dst6)) {
        l3_ipv6_interface_t *chosen = NULL;
        uint8_t cnt = l2_interface_count();
        for (uint8_t li = 0; li < cnt; li++) {
            l2_interface_t *l2 = l2_interface_at(li);
            if (!l2 || !l2->is_up) continue;
            for (int vi = 0; vi < MAX_IPV6_PER_INTERFACE; vi++) {
                l3_ipv6_interface_t *v6 = l2->l3_v6[vi];
                if (!ipv6_l3_is_ready(v6) || v6->is_localhost || !ipv6_is_linklocal(v6->ip)) continue;
                if (chosen) {
                    print("ping: IPv6 link local multicast requires -s when more IPv6 interfaces are usable");
                    return 2;
                }
                chosen = v6;
            }
        }
        if (!chosen) {
            print("ping: no usable IPv6 interface");
            return 2;
        }
        txo.index = (uint8_t)chosen->l3_id;
        txo.scope = IP_TX_BOUND_L3;
        txop = &txo;
    }

    bool multi = ipv6_is_multicast(dst6);
    uint32_t max_results = multi ? PING_MAX_REPLIES : 1;
    for (uint32_t i = 0; i < o->count; i++) {
        ++sent;
        uint16_t seq = (uint16_t)(seq_base + i);

        ping6_result_t res[PING_MAX_REPLIES];
        uint32_t n = icmpv6_ping_collect(dst6, id, seq, o->timeout_ms, txop, (uint8_t)o->ttl, res, max_results);

        if (n) {
            for (uint32_t j = 0; j < n; j++) {
                if (res[j].status == PING_OK) {
                    ++received;
                    uint32_t rtt = res[j].rtt_ms;
                    if (rtt < min_ms) min_ms = rtt;
                    if (rtt > max_ms) max_ms = rtt;
                    sum_ms += rtt;
                    char rip[64];
                    ipv6_to_string(res[j].responder_ip, rip, (int)sizeof(rip));
                    print("Reply from %s: bytes=32 time=%ums", rip, rtt);
                } else print("%s", status_to_msg(res[j].status));
            }
        } else print("%s", status_to_msg(PING_TIMEOUT));

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

    if (opts.addr.src_ip.ver && opts.addr.src_ip.ver != opts.ver) {
        print("ping: source address version doesn't match target version");
        return 2;
    }

    if (opts.ver == IP_VER4 && opts.addr.src_ip.ver) {
        uint32_t src4 = 0;
        memcpy(&src4, opts.addr.src_ip.ip, 4);
        if (!l3_ipv4_find_by_ip(src4)) {
            char ssrc[16];
            ipv4_to_string(src4, ssrc);
            print("ping: invalid source %s (no local ip match)", ssrc);
            return 2;
        }
    }

    if (opts.ver == IP_VER6 && opts.addr.src_ip.ver && !l3_ipv6_find_by_ip(opts.addr.src_ip.ip)) {
        char ssrc[64];
        ipv6_to_string(opts.addr.src_ip.ip, ssrc, (int)sizeof(ssrc));
        print("ping: invalid source %s (no local ip match)", ssrc);
        return 2;
    }

    if (opts.ver == IP_VER4 && opts.addr.src_if.scope == IP_TX_BOUND_L3) {
        if (!ipv4_l3_is_ready(l3_ipv4_find_by_id(opts.addr.src_if.index))) {
            print("ping: invalid IPv4 L3 interface %u", opts.addr.src_if.index);
            return 2;
        }
    }

    if (opts.ver == IP_VER6 && opts.addr.src_if.scope == IP_TX_BOUND_L3) {
        if (!ipv6_l3_is_ready(l3_ipv6_find_by_id(opts.addr.src_if.index))) {
            print("ping: invalid IPv6 L3 interface %u", opts.addr.src_if.index);
            return 2;
        }
    }

    if (opts.addr.src_if.scope == IP_TX_BOUND_L2) {
        l2_interface_t *l2 = l2_interface_find_by_index(opts.addr.src_if.index);
        if (!l2 || !l2->is_up) {
            print("ping: invalid L2 interface %u", opts.addr.src_if.index);
            return 2;
        }

        bool has_l3 = false;
        if (opts.ver == IP_VER4) {
            for (int i = 0; i < MAX_IPV4_PER_INTERFACE; i++) {
                if (ipv4_l3_is_ready(l2->l3_v4[i])) {
                    has_l3 = true;
                    break;
                }
            }
        } else {
            for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
                if (ipv6_l3_is_ready(l2->l3_v6[i])) {
                    has_l3 = true;
                    break;
                }
            }
        }
        if (!has_l3) {
            print("ping: L2 interface %u has no usable %s L3 interface", opts.addr.src_if.index, opts.ver == IP_VER4 ? "IPv4" : "IPv6");
            return 2;
        }
    }

    if (opts.ver == IP_VER4) return ping_v4(&opts);
    if (opts.ver == IP_VER6) return ping_v6(&opts);
    print("usage: ping [-4/-6] [-n times] [-w timeout] [-i interval] [-t TTL] [-s ip|l2:id|l3:id] host");

    return 2;
}
