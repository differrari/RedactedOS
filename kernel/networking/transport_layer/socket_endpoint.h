#pragma once

#include "types.h"
#include "net/network_types.h"
#include "networking/application_layer/dns/dns.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t socket_endpoint_resolve(const char* host, uint16_t port, dns_server_sel_t sel, uint32_t timeout_ms, net_l4_endpoint* out, uint32_t max, dns_result_t* err);
bool socket_endpoint_select(const char* host, uint16_t port, ip_version_t ver, dns_server_sel_t sel, uint32_t timeout_ms, net_l4_endpoint* out, dns_result_t* err);

#ifdef __cplusplus
}
#endif
