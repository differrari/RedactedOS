#pragma once

#include "types.h"
#include "net/network_types.h"
#include "net/socket_types.h"
#include "networking/transport_layer/socket_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef SocketHandle socket_handle_t;

bool create_socket(Socket_Role role, protocol_t protocol, const SocketExtraOptions* extra, SocketHandle* out_handle);
int32_t bind_socket(SocketHandle *handle, uint16_t port, ip_version_t ip_vers);
int32_t bind_socket_spec(SocketHandle *handle, const SockBindSpec* spec, uint16_t port);
int32_t connect_socket(SocketHandle *handle, const net_l4_endpoint* dst);

int64_t send_on_socket(SocketHandle *handle, const void* buf, uint64_t len);
int64_t send_to_socket(SocketHandle *handle, const net_l4_endpoint* dst, const void* buf, uint64_t len);
int64_t receive_from_socket(SocketHandle *handle, void* buf, uint64_t len, net_l4_endpoint* out_src);
int32_t set_socket_option(SocketHandle *handle, int32_t opt, const void* value, uint32_t len);
int32_t get_socket_option(SocketHandle *handle, int32_t opt, void* value, uint32_t* len);

uint16_t get_socket_local_port(SocketHandle *handle);
bool get_socket_remote_endpoint(SocketHandle *handle, net_l4_endpoint* out);
int32_t close_socket(SocketHandle *handle);

int32_t listen_on(SocketHandle *handle, int32_t backlog);
bool accept_on_socket(SocketHandle *handle, SocketHandle* out_child);

#ifdef __cplusplus
}
#endif