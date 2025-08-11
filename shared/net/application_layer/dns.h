#pragma once
#include "net/transport_layer/csocket_udp.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    DNS_OK = 0,
    DNS_ERR_NO_DNS = -1,
    DNS_ERR_SOCKET = -2,
    DNS_ERR_SEND = -3,
    DNS_ERR_TIMEOUT = -4,
    DNS_ERR_FORMAT = -5,
    DNS_ERR_NXDOMAIN = -6,
    DNS_ERR_NO_ANSWER = -7
} dns_result_t;

typedef enum {
    DNS_USE_PRIMARY = 0,
    DNS_USE_SECONDARY = 1,
    DNS_USE_BOTH = 2
} dns_server_sel_t;

dns_result_t dns_resolve_a(const char* hostname, uint32_t* out_ip, dns_server_sel_t which, uint32_t timeout_ms);
#ifdef __cplusplus
}
#endif