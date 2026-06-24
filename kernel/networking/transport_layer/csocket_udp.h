#pragma once
#include "types.h"
#include "net/network_types.h"
#include "socket_core.h"
#include "net/socket_types.h"
#include "networking/netpkt.h"

#ifdef __cplusplus
extern "C" {
#endif

socket_impl_t udp_socket_create(ksocket_t* owner, const SocketOptions* extra);
int32_t socket_bind_udp(socket_impl_t sh, const SockBindSpec* spec, uint16_t port);
int32_t socket_connect_udp(socket_impl_t sh, const net_l4_endpoint* dst);
int64_t socket_sendto_udp(socket_impl_t sh, const net_l4_endpoint* dst, const void* buf, uint64_t len);
int64_t socket_recvfrom_udp(socket_impl_t sh, void* buf, uint64_t len, net_l4_endpoint* out_src);
int32_t socket_close_udp(socket_impl_t sh);
void socket_destroy_udp(socket_impl_t sh);
int32_t socket_setopt_udp(socket_impl_t sh, int32_t opt, const void* value, uint32_t len);
int32_t socket_getopt_udp(socket_impl_t sh, int32_t opt, void* value, uint32_t* len);
uint32_t socket_udp_input(ksocket_t* socket, ip_version_t ipver, uint8_t l3_id, const void* src_ip_addr, const void* dst_ip_addr, netpkt_t* pkt, uint16_t src_port, uint16_t dst_port);

#ifdef __cplusplus
}
#endif
