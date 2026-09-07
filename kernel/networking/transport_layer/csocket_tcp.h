#pragma once
#include "types.h"
#include "net/network_types.h"
#include "socket_core.h"
#include "net/socket_types.h"

#ifdef __cplusplus
extern "C" {
#endif

socket_impl_t socket_tcp_create(ksocket_t* owner, const SocketOptions* extra);
int32_t socket_bind_tcp(socket_impl_t sh, const SockBindSpec* spec, uint16_t port);
int32_t socket_listen_tcp(socket_impl_t sh, int32_t backlog);
ksocket_t* socket_accept_tcp(socket_impl_t sh);
int32_t socket_connect_tcp(socket_impl_t sh, const net_l4_endpoint* dst);
int64_t socket_send_tcp(socket_impl_t sh, const void* buf, uint64_t len);
int64_t socket_recv_tcp(socket_impl_t sh, void* buf, uint64_t len);
int32_t socket_close_tcp(socket_impl_t sh);
int32_t socket_setopt_tcp(socket_impl_t sh, int32_t opt, const void* value, uint32_t len);
int32_t socket_getopt_tcp(socket_impl_t sh, int32_t opt, void* value, uint32_t* len);
void socket_destroy_tcp(socket_impl_t sh);
const SocketOptions* socket_tcp_options(socket_impl_t sh);
uint32_t tcp_accept_enqueue(ksocket_t* listener, ip_version_t ipver, const void* src_ip_addr, const void* dst_ip_addr, uint16_t src_port, uint16_t dst_port);

#ifdef __cplusplus
}
#endif
