#include "socket_endpoint.h"
#include "networking/transport_layer/trans_utils.h"
#include "std/memory.h"

uint32_t socket_endpoint_resolve(const char* host, uint16_t port, dns_server_sel_t sel, uint32_t timeout_ms, net_l4_endpoint* out) {
    if (!host || !port || !out) return 0;

    memset(out, 0, sizeof(net_l4_endpoint) * 2);

    uint32_t count = 0;
    uint8_t v6addr[16];
    uint32_t v4addr = 0;

    memset(v6addr, 0, sizeof(v6addr));
    if (dns_resolve_aaaa(host, v6addr, sel, timeout_ms) == DNS_OK) {
        out[count].ver = IP_VER6;
        memcpy(out[count].ip, v6addr, 16);
        out[count].port = port;
        count++;
    }

    if (dns_resolve_a(host, &v4addr, sel, timeout_ms) == DNS_OK) {
        make_ep(&v4addr, port, IP_VER4, &out[count]);
        count++;
    }

    return count;
}

net_l4_endpoint socket_endpoint_select(const char* host, uint16_t port, ip_version_t preferred, dns_server_sel_t sel, uint32_t timeout_ms) {
    net_l4_endpoint endpoints[2];
    memset(endpoints, 0, sizeof(endpoints));

    uint32_t count = socket_endpoint_resolve(host, port, sel, timeout_ms, endpoints);
    if (!count) return (net_l4_endpoint){};

    if (preferred == IP_VER4 || preferred == IP_VER6) {
        for (uint32_t i = 0; i < count; ++i) if (endpoints[i].ver == preferred) return endpoints[i];
    }

    return endpoints[0];
}
