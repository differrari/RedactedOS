#pragma once

#include "types.h"
#include "net/network_types.h"
#include "net/socket_types.h"

bool create_socket(Socket_Role role, protocol_t protocol, const SocketExtraOptions* extra, uint16_t pid, SocketHandle *out_handle);
int32_t bind_socket(SocketHandle *handle, uint16_t port, ip_version_t ip_vers, uint16_t pid);
int32_t connect_socket(SocketHandle *handle, uint8_t dst_kind, const void* dst, uint16_t port, uint16_t pid);

int64_t send_on_socket(SocketHandle *sh, uint8_t dst_kind, const void* dst, uint16_t port, void* buf, uint64_t len, uint16_t pid);
int64_t receive_from_socket(SocketHandle *sh, void* buf, uint64_t len, net_l4_endpoint* out_src, uint16_t pid);
int32_t close_socket(SocketHandle *sh, uint16_t pid);

int32_t listen_on(SocketHandle *sh, int32_t backlog, uint16_t pid);
void accept_on_socket(SocketHandle *sh, uint16_t pid);