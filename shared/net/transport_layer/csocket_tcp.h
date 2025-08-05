#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* socket_handle_t;

socket_handle_t socket_tcp_create(uint8_t role, uint32_t pid);
int32_t socket_bind_tcp(socket_handle_t sh, uint16_t port);
int32_t socket_listen_tcp(socket_handle_t sh, int32_t backlog);
socket_handle_t socket_accept_tcp(socket_handle_t sh);
int32_t socket_connect_tcp(socket_handle_t sh, uint32_t ip, uint16_t port);
int64_t socket_send_tcp(socket_handle_t sh, const void* buf, uint64_t len);
int64_t socket_recv_tcp(socket_handle_t sh, void* buf, uint64_t len);
int32_t socket_close_tcp(socket_handle_t sh);
void socket_destroy_tcp(socket_handle_t sh);

uint16_t socket_get_local_port_tcp(socket_handle_t sh);
uint32_t socket_get_remote_ip_tcp(socket_handle_t sh);
uint16_t socket_get_remote_port_tcp(socket_handle_t sh);
uint8_t socket_get_protocol_tcp(socket_handle_t sh);
uint8_t socket_get_role_tcp(socket_handle_t sh);
bool socket_is_bound_tcp(socket_handle_t sh);
bool socket_is_connected_tcp(socket_handle_t sh);

#ifdef __cplusplus
}
#endif
