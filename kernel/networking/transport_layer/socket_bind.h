#pragma once

#include "types.h"
#include "net/network_types.h"
#include "net/socket_types.h"
#include "networking/transport_layer/socket_core.h"
#include "networking/internet_layer/ipv4_route.h"
#include "networking/internet_layer/ipv6_route.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOCKET_BIND_MAX 1024
#define SOCKET_BIND_COLLECT_MAX 32
typedef uint32_t socket_bind_token_t;

bool socket_bind_normalize_spec(SockBindSpec* spec);
bool socket_bind_insert(ksocket_t* socket, protocol_t protocol, const SockBindSpec* spec, uint16_t port, socket_bind_token_t* out_token);
void socket_bind_remove(socket_bind_token_t token);
int socket_bind_alloc_ephemeral(ksocket_t* socket, protocol_t protocol, const SockBindSpec* spec, socket_bind_token_t* out_token);
int socket_bind_alloc_ephemeral_l3(ksocket_t* socket, protocol_t protocol, uint8_t l3_id, socket_bind_token_t* out_token);

uint32_t socket_bind_collect(protocol_t protocol, ip_version_t ipver, uint8_t l3_id, uint8_t ifindex, const void* dst_ip_addr, uint16_t dst_port, ksocket_t** out, uint32_t out_cap);

uint32_t socket_bind_select_l3(const SockBindSpec* spec, ip_version_t ver, uint8_t* out, uint32_t cap);
bool socket_bind_build_ipv4_tx_plan(const SockBindSpec* spec, bool use_spec, uint32_t dst, ipv4_tx_plan_t* out);
bool socket_bind_build_ipv6_tx_plan(const SockBindSpec* spec, bool use_spec, const uint8_t dst[16], ipv6_tx_plan_t* out);

#ifdef __cplusplus
}
#endif
