#include "socket_endpoint.h"
#include "networking/transport_layer/trans_utils.h"

uint32_t socket_endpoint_resolve(const char* host, uint16_t port, dns_server_sel_t sel, uint32_t timeout_ms, net_l4_endpoint* out, uint32_t max, dns_result_t* err) {
    if (err) *err = DNS_ERR_FORMAT;
    if (!host || !host[0] || !out || !max) return 0;

    uint32_t count = 0;
    dns_result_t err6 = DNS_ERR_NO_ANSWER;
    dns_result_t err4 = DNS_ERR_NO_ANSWER;
    net_l4_endpoint ep;

    if (socket_endpoint_select(host, port, IP_VER6, sel, timeout_ms, &ep, &err6)) out[count++] = ep;
    if (count < max && socket_endpoint_select(host, port, IP_VER4, sel, timeout_ms, &ep, &err4)) out[count++] = ep;

    if (err) { 
        if (count) *err = DNS_OK;
        else if (err6 != DNS_ERR_NO_ANSWER) *err = err6;
        else *err = err4;
    }

    return count;
}

bool socket_endpoint_select(const char* host, uint16_t port, ip_version_t ver, dns_server_sel_t sel, uint32_t timeout_ms, net_l4_endpoint* out, dns_result_t* err) {
    if (err) *err = DNS_ERR_FORMAT;
    if (!host || !host[0] || !out || (ver != IP_VER4 && ver != IP_VER6)) return false; 

    uint32_t v4;
    if (ipv4_parse(host,&v4)) {
        if (ver != IP_VER4) {
            if (err) *err = DNS_ERR_NO_ANSWER;
            return false;
        }
        make_ep(&v4, port, IP_VER4, out);
        if (err) *err = DNS_OK;
        return true;
    }

    uint8_t v6[16];
    if (ipv6_parse(host, v6)) {
        if (ver != IP_VER6) {
            if (err) *err = DNS_ERR_NO_ANSWER;
            return false;
        }
        make_ep(v6, port, IP_VER6, out);
        if (err) *err = DNS_OK;
        return true;
    }

    dns_result_t res;
    if (ver == IP_VER4) res = dns_resolve_a(host, &v4, sel, timeout_ms);
    else res = dns_resolve_aaaa(host, v6, sel, timeout_ms);
    if (err) *err = res;
    if (res != DNS_OK) return false;

    if (ver == IP_VER4) make_ep(&v4, port, IP_VER4, out);
    else make_ep(v6, port, IP_VER6, out);
    return true;
}
