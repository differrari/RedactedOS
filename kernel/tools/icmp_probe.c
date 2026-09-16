#include "icmp_probe.h"
#include "net/checksums.h"
#include "std/memory.h"
#include "std/string.h"
#include "networking/internet_layer/icmp.h"
#include "networking/internet_layer/icmpv6.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/transport_layer/csocket.h"
#include "syscalls/syscalls.h"

#define ICMP_PROBE_PAYLOAD_LEN 32

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
    uint8_t payload[ICMP_PROBE_PAYLOAD_LEN];
} icmp_probe4_echo_t;

typedef struct __attribute__((packed)) {
    icmpv6_hdr_t hdr;
    uint16_t id;
    uint16_t seq;
    uint8_t payload[ICMP_PROBE_PAYLOAD_LEN];
} icmp_probe6_echo_t;

bool icmp_probe_parse_bind(const char* arg, SockBindSpec* out){
    if (!arg || !out) return false;

    memset(out, 0, sizeof(*out));
    uint32_t id = 0;
    if (strncmp(arg, "l2:", 3) == 0) {
        if (!parse_uint32_dec_exact(arg + 3, &id) || id == 0 || id > UINT8_MAX) return false;
        out->kind = BIND_L2;
        out->ifindex = (uint8_t)id;
        return true;
    }

    if (strncmp(arg, "l3:", 3) == 0) {
        if (!parse_uint32_dec_exact(arg + 3, &id) || id == 0 || id > UINT16_MAX) return false;
        out->kind = BIND_L3;
        out->l3_id = (l3_id_t)id;
        return true;
    }

    uint32_t v4 = 0;
    if (ipv4_parse(arg, &v4)) {
        out->kind = BIND_IP;
        out->ver = IP_VER4;
        memcpy(out->ip, &v4, sizeof(v4));
        return true;
    }

    if (ipv6_parse(arg, out->ip)) {
        out->kind = BIND_IP;
        out->ver = IP_VER6;
        return true;
    }

    return false;
}

uint32_t icmp_probe_collect(const net_l4_endpoint* dst, uint16_t id, uint16_t seq, uint32_t timeout_ms, const SockBindSpec* bind, uint8_t ttl, icmp_probe_result_t* out, uint32_t max_results){
    if (!dst || !out || !max_results) return 0;
    if (dst->ver != IP_VER4 && dst->ver != IP_VER6) return 0;

    SocketOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.special_kind = SOCKET_SPECIAL_RAW;
    opt.flags = SOCK_OPT_SPECIAL | SOCK_OPT_FILTER | SOCK_OPT_NONBLOCK;
    opt.raw_filter.count = 5;
    if (dst->ver == IP_VER4) {
        opt.raw_filter.rules[0].type = ICMP_ECHO_REPLY;
        opt.raw_filter.rules[0].code = 0;
        opt.raw_filter.rules[0].flags = SOCKET_RAW_FILTER_HAS_CODE;
        opt.raw_filter.rules[1].type = ICMP_DEST_UNREACH;
        opt.raw_filter.rules[2].type = ICMP_TIME_EXCEEDED;
        opt.raw_filter.rules[3].type = ICMP_PARAM_PROBLEM;
        opt.raw_filter.rules[4].type = ICMP_REDIRECT;
    } else {
        opt.raw_filter.rules[0].type = ICMPV6_ECHO_REPLY;
        opt.raw_filter.rules[0].code = 0;
        opt.raw_filter.rules[0].flags = SOCKET_RAW_FILTER_HAS_CODE;
        opt.raw_filter.rules[1].type = ICMPV6_DEST_UNREACH;
        opt.raw_filter.rules[2].type = ICMPV6_PACKET_TOO_BIG;
        opt.raw_filter.rules[3].type = ICMPV6_TIME_EXCEEDED;
        opt.raw_filter.rules[4].type = ICMPV6_PARAM_PROBLEM;
    }
    if (ttl) {
        opt.flags |= SOCK_OPT_TTL;
        opt.ttl = ttl;
    }

    protocol_t proto = dst->ver == IP_VER4 ? PROTO_ICMP : PROTO_ICMPV6;
    socket_handle_t sock = create_socket(proto, &opt);
    if (!sock) return 0;

    if (bind) {
        SockBindSpec spec = *bind;
        if (spec.kind == BIND_L3) spec.ver = dst->ver;
        if (bind_socket(sock, &spec, 0) != SOCK_OK) {
            close_socket(sock);
            return 0;
        }
    }

    int64_t sent = 0;
    uint32_t tx_len = 0;
    if (dst->ver == IP_VER4) {
        icmp_probe4_echo_t echo;
        memset(&echo, 0, sizeof(echo));
        echo.type = ICMP_ECHO_REQUEST;
        echo.id = bswap16(id);
        echo.seq = bswap16(seq);
        echo.checksum = bswap16(checksum16(&echo, sizeof(echo)));
        tx_len = (uint32_t)sizeof(echo);
        sent = send_to_socket(sock, dst, &echo, sizeof(echo));
    } else {
        icmp_probe6_echo_t echo;
        memset(&echo, 0, sizeof(echo));
        echo.hdr.type = ICMPV6_ECHO_REQUEST;
        echo.id = bswap16(id);
        echo.seq = bswap16(seq);
        tx_len = (uint32_t)sizeof(echo);
        sent = send_to_socket(sock, dst, &echo, sizeof(echo));
    }

    if (sent != (int64_t)tx_len) {
        close_socket(sock);
        return 0;
    }

    uint32_t count = 0;
    uint32_t start = (uint32_t)get_time();
    while (count < max_results) { 
        uint32_t now = (uint32_t)get_time();
        if (now - start >= timeout_ms) break;

        uint8_t rx[1280];
        net_l4_endpoint src;
        memset(&src, 0, sizeof(src));
        int64_t n = receive_from_socket(sock, rx, sizeof(rx), &src);
        if (n == SOCK_ERR_WOULDBLOCK) {
            msleep(5);
            continue;
        }
        if (n < 8) {
            if (n < 0) msleep(5);
            continue;
        }

        uint8_t type = rx[0];
        uint8_t code = rx[1];
        uint8_t status = ICMP_PROBE_UNKNOWN_ERROR;
        bool matched = false;

        if (dst->ver == IP_VER4) {
            if (type == ICMP_ECHO_REPLY && rd_be16(rx + 4) == id && rd_be16(rx + 6) == seq) matched = true;
            else if ((type == ICMP_TIME_EXCEEDED || type == ICMP_DEST_UNREACH || type == ICMP_PARAM_PROBLEM || type == ICMP_REDIRECT) && (uint32_t)n >= 8 + sizeof(ipv4_hdr_t) + 8) {
                const uint8_t* inner_ip = rx + 8;
                uint8_t ihl = (uint8_t)(inner_ip[0] & 0x0F);
                uint32_t iphdr = (uint32_t)ihl * 4;
                if ((inner_ip[0] >> 4) == IP_VER4 && ihl >= IP_IHL_NOOPTS && (uint32_t)n >= 8 + iphdr + 8 && inner_ip[9] == PROTO_ICMP) {
                    const uint8_t* inner_icmp = inner_ip + iphdr;
                    matched = (inner_icmp[0] == ICMP_ECHO_REQUEST || inner_icmp[0] == ICMP_ECHO_REPLY) && rd_be16(inner_icmp + 4) == id && rd_be16(inner_icmp + 6) == seq;
                }
            }
            if (!matched) continue;

            switch (type) {
                case ICMP_ECHO_REPLY: status = ICMP_PROBE_OK; break;
                case ICMP_DEST_UNREACH:
                    switch (code) {
                        case 0: status = ICMP_PROBE_NET_UNREACH; break;
                        case 1: status = ICMP_PROBE_HOST_UNREACH; break;
                        case 2: status = ICMP_PROBE_PROTO_UNREACH; break;
                        case 3: status = ICMP_PROBE_PORT_UNREACH; break;
                        case 4: status = ICMP_PROBE_FRAG_NEEDED; break;
                        case 5: status = ICMP_PROBE_SRC_ROUTE_FAILED; break;
                        case 13: status = ICMP_PROBE_ADMIN_PROHIBITED; break;
                        default: status = ICMP_PROBE_UNKNOWN_ERROR; break;
                    }
                    break;
                case ICMP_TIME_EXCEEDED: status = ICMP_PROBE_TTL_EXPIRED; break;
                case ICMP_PARAM_PROBLEM: status = ICMP_PROBE_PARAM_PROBLEM; break;
                case ICMP_REDIRECT: status = ICMP_PROBE_REDIRECT; break;
                default: break;
            }
        } else {
            if (type == ICMPV6_ECHO_REPLY && rd_be16(rx + 4) == id && rd_be16(rx + 6) == seq) matched = true;
            else if ((type == ICMPV6_DEST_UNREACH || type == ICMPV6_PACKET_TOO_BIG || type == ICMPV6_TIME_EXCEEDED || type == ICMPV6_PARAM_PROBLEM) && (uint32_t)n >= 8 + sizeof(ipv6_hdr_t) + 8) {
                ipv6_hdr_t inner;
                memcpy(&inner, rx + 8, sizeof(inner));
                uint32_t v = bswap32(inner.ver_tc_fl);
                const uint8_t* inner_icmp = rx + 8 + sizeof(ipv6_hdr_t);
                matched = (v >> 28) == IP_VER6 && inner.next_header == PROTO_ICMPV6 && inner_icmp[0] == ICMPV6_ECHO_REQUEST && rd_be16(inner_icmp + 4) == id && rd_be16(inner_icmp + 6) == seq;
            }
            if (!matched) continue;

            switch (type) { //b
                case ICMPV6_ECHO_REPLY: status = ICMP_PROBE_OK; break;
                case ICMPV6_DEST_UNREACH:
                    switch (code) {
                        case 0:status = ICMP_PROBE_NET_UNREACH; break;
                        case 1:
                        case 2:status = ICMP_PROBE_ADMIN_PROHIBITED; break;
                        case 3:status = ICMP_PROBE_HOST_UNREACH; break;
                        case 4:status = ICMP_PROBE_PORT_UNREACH; break;
                        default: status = ICMP_PROBE_UNKNOWN_ERROR; break;
                    }
                    break;
                case ICMPV6_PACKET_TOO_BIG: status = ICMP_PROBE_FRAG_NEEDED; break;
                case ICMPV6_TIME_EXCEEDED: status = ICMP_PROBE_TTL_EXPIRED; break;
                case ICMPV6_PARAM_PROBLEM: status = ICMP_PROBE_PARAM_PROBLEM; break;
                default: break;
            }
        }

        icmp_probe_result_t* r = &out[count++];
        memset(r, 0, sizeof(*r));
        r->responder = src;
        r->icmp_type = type;
        r->icmp_code = code;
        r->status = status;
        now = (uint32_t)get_time();
        r->rtt_ms = now >= start ? now - start : 0;
        if (max_results == 1) break;
    }

    close_socket(sock);
    return count;
}
