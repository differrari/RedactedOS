#pragma once

#include "socket_core.h"
#include "networking/netpkt.h"

#ifdef __cplusplus
extern "C" {
#endif

socket_impl_t socket_raw_create(ksocket_t* owner, const SocketOptions* extra);
void socket_destroy_raw(socket_impl_t sh);
int32_t socket_close_raw(socket_impl_t sh);
int32_t socket_setopt_raw(socket_impl_t sh, int32_t opt, const void* value, uint32_t len);
int32_t socket_getopt_raw(socket_impl_t sh, int32_t opt, void* value, uint32_t* len);
int32_t socket_bind_raw(socket_impl_t sh, const SockBindSpec* spec);
int32_t socket_connect_raw(socket_impl_t sh, const net_l4_endpoint* dst);
int64_t socket_send_raw(socket_impl_t sh, const void* buf, uint64_t len);
int64_t socket_sendto_raw(socket_impl_t sh, const net_l4_endpoint* dst, const void* buf, uint64_t len);
int64_t socket_recv_raw(socket_impl_t sh, void* buf, uint64_t len, net_l4_endpoint* out_src);
bool socket_raw_input_v4(protocol_t protocol, uint8_t ifindex, uint32_t src, uint32_t dst, netpkt_t* pkt);
bool socket_raw_input_v6(uint8_t ifindex, const uint8_t src[16], const uint8_t dst[16], netpkt_t* pkt);

#ifdef __cplusplus
}
#endif
