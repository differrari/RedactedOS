#pragma once

#include "types.h"
#include "net/network_types.h"
#include "net/socket_types.h"
#include "networking/transport_layer/socket_core.h"

#ifdef __cplusplus
extern "C" {
#endif

socket_handle_t create_socket(protocol_t protocol, const SocketExtraOptions* extra);
int32_t bind_socket(socket_handle_t handle, const SockBindSpec* spec, uint16_t port);
int32_t connect_socket(socket_handle_t handle, const net_l4_endpoint* dst);

int64_t send_on_socket(socket_handle_t handle, const void* buf, uint64_t len);
int64_t send_to_socket(socket_handle_t handle, const net_l4_endpoint* dst, const void* buf, uint64_t len);
int64_t receive_from_socket(socket_handle_t handle, void* buf, uint64_t len, net_l4_endpoint* out_src);
int32_t set_socket_option(socket_handle_t handle, int32_t opt, const void* value, uint32_t len);
int32_t get_socket_option(socket_handle_t handle, int32_t opt, void* value, uint32_t* len);

uint16_t get_socket_local_port(socket_handle_t handle);
bool get_socket_remote_endpoint(socket_handle_t handle, net_l4_endpoint* out);
int32_t close_socket(socket_handle_t handle);

int32_t listen_on(socket_handle_t handle, int32_t backlog);
socket_handle_t accept_on_socket(socket_handle_t handle);

#ifdef __cplusplus
}
#endif