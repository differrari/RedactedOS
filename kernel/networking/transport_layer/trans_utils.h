#pragma once
#include "types.h"
#include "net/network_types.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "std/std.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline void net_ep_split(const net_l4_endpoint* ep, char* ip, int iplen, bool* is_v6, uint16_t* port) {
    if (ip && iplen > 0) {
        ip[0] = '-';
        if (iplen > 1) ip[1] = 0;
    }
    if (is_v6) *is_v6 = false;
    if (port) *port = 0;
    if (!ep || !ip || iplen<= 0) return;

    if (port)*port = ep->port;
    if (ep->ver ==IP_VER4) {
        uint32_t v4 = 0;
        memcpy(&v4, ep->ip, 4);
        ipv4_to_string(v4, ip);
        if (is_v6) *is_v6 = false;
        return;
    }

    if (ep->ver ==IP_VER6) {
        ipv6_to_string(ep->ip, ip, iplen);
        if (is_v6) *is_v6 = true;
        return;
    }
}

static void make_ep(uint32_t ip_host, uint16_t port, ip_version_t ver, net_l4_endpoint* ep) {
    memset(ep, 0, sizeof(*ep));
    ep->ver = ver;
    memcpy(ep->ip, &ip_host, 4);
    ep->port = port;
}

#ifdef __cplusplus
}
#endif