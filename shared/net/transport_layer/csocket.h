#pragma once

#include "types.h"
#include "net/network_types.h"

bool create_socket(Socket_Role role, protocol_t protocol, uint16_t pid, SocketHandle *out_handle);
int32_t bind_socket(SocketHandle *handle, uint16_t port);
int32_t connect_socket(SocketHandle *handle, uint8_t dst_kind, const void* dst, uint16_t port);

int64_t send_on_socket(SocketHandle *sh, uint8_t dst_kind, const void* dst, uint16_t port, sizedptr ptr);
int64_t receive_from_socket(SocketHandle *sh, void* buf, uint64_t len, net_l4_endpoint* out_src);
int32_t close_socket(SocketHandle *sh);

int32_t listen_on(SocketHandle *sh, int32_t backlog);
void accept_on_socket(SocketHandle *sh);