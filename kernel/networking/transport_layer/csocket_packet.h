#pragma once

#include "socket_core.h"
#include "networking/netpkt.h"

#ifdef __cplusplus
extern "C" {
#endif

socket_impl_t socket_packet_create(ksocket_t* owner, const SocketOptions* extra);
void socket_destroy_packet(socket_impl_t sh);
int32_t socket_close_packet(socket_impl_t sh);
int32_t socket_setopt_packet(socket_impl_t sh, int32_t opt, const void* value, uint32_t len);
int32_t socket_getopt_packet(socket_impl_t sh, int32_t opt, void* value, uint32_t* len);
int32_t socket_bind_packet(socket_impl_t sh, const SockBindSpec* spec);
int64_t socket_recv_packet(socket_impl_t sh, void* buf, uint64_t len);
bool socket_packet_input(uint8_t ifindex, netpkt_t* pkt);

#ifdef __cplusplus
}
#endif
