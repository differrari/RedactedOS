#pragma once
#include "types.h"
#include "net/network_types.h"
#include "networking/transport_layer/socket.hpp"
#include "net/socket_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* socket_handle_t;

socket_handle_t udp_socket_create(uint8_t role, uint32_t pid, const SocketExtraOptions* extra);
int32_t socket_bind_udp_ex(socket_handle_t sh, const SockBindSpec* spec, uint16_t port);
int64_t socket_sendto_udp_ex(socket_handle_t sh, uint8_t dst_kind, const void* dst, uint16_t port, const void* buf, uint64_t len);
int64_t socket_recvfrom_udp_ex(socket_handle_t sh, void* buf, uint64_t len, net_l4_endpoint* out_src);
int32_t socket_close_udp(socket_handle_t sh);
void socket_destroy_udp(socket_handle_t sh);

uint16_t socket_get_local_port_udp(socket_handle_t sh);
uint16_t socket_get_remote_port_udp(socket_handle_t sh);
void socket_get_remote_ep_udp(socket_handle_t sh, net_l4_endpoint* out);

uint8_t socket_get_protocol_udp(socket_handle_t sh);
uint8_t socket_get_role_udp(socket_handle_t sh);
bool socket_is_bound_udp(socket_handle_t sh);
bool socket_is_connected_udp(socket_handle_t sh);

#ifdef __cplusplus
}
#endif
