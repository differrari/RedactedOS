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
typedef uint32_t socket_bind_token_t;

bool socket_bind_prepare_spec(SockBindSpec* spec, protocol_t protocol);
uint8_t socket_bind_match_score(const SockBindSpec* spec, ip_version_t ver, uint8_t l3_id, uint8_t ifindex, const void* ip_addr);
bool socket_bind_insert(ksocket_t* socket, protocol_t protocol, SockBindSpec* spec, uint16_t port, uint32_t options, bool allow_reuse, socket_bind_token_t* out_token);
bool socket_bind_tcp_listen(socket_bind_token_t token);
void socket_bind_remove(socket_bind_token_t token);
void socket_bind_udp_set_remote(socket_bind_token_t token, const net_l4_endpoint* remote);
int32_t socket_bind_alloc_ephemeral(ksocket_t* socket, protocol_t protocol, SockBindSpec* spec, uint32_t options, socket_bind_token_t* out_token);
int32_t socket_bind_alloc_ephemeral_l3(ksocket_t* socket, protocol_t protocol, uint8_t l3_id, uint32_t options, SockBindSpec* out_spec, socket_bind_token_t* out_token);

ksocket_t* socket_bind_lookup(protocol_t protocol, ip_version_t ipver, uint8_t l3_id, uint8_t ifindex, const void* src_ip_addr, uint16_t src_port, const void* dst_ip_addr, uint16_t dst_port);
ksocket_t* socket_bind_udp_next_fanout(ip_version_t ipver, uint8_t l3_id, uint8_t ifindex, const void* dst_ip_addr, uint16_t dst_port, uint32_t* cursor);

uint32_t socket_bind_select_l3(const SockBindSpec* spec, ip_version_t ver, uint8_t* out, uint32_t cap);
bool socket_bind_build_ipv4_tx_plan(const SockBindSpec* spec, bool use_spec, uint32_t dst, ipv4_tx_plan_t* out);
bool socket_bind_build_ipv6_tx_plan(const SockBindSpec* spec, bool use_spec, const uint8_t dst[16], ipv6_tx_plan_t* out);

#ifdef __cplusplus
}
#endif
