#include "networking/transport_layer/socket_udp.hpp"
#include "networking/transport_layer/socket.hpp"
#include "csocket_udp.h"

extern "C" {
    
socket_impl_t udp_socket_create(ksocket_t* owner, uint8_t role, uint32_t pid, const SocketExtraOptions* extra) {
    if (!owner) return nullptr;
    return reinterpret_cast<socket_impl_t>(new UDPSocket(owner, role, pid, extra));
}

int32_t socket_bind_udp(socket_impl_t sh, const SockBindSpec* spec, uint16_t port) {
    if (!sh || !spec) return SOCK_ERR_INVAL;
    return reinterpret_cast<UDPSocket*>(sh)->bind(*spec, port);
}

int64_t socket_sendto_udp(socket_impl_t sh, const net_l4_endpoint* dst, const void* buf, uint64_t len) {
    if (!sh || !buf || !len) return SOCK_ERR_INVAL;
    return reinterpret_cast<UDPSocket*>(sh)->sendto(dst, buf, len);
}

int64_t socket_recvfrom_udp(socket_impl_t sh, void* buf, uint64_t len, net_l4_endpoint* out_src) {
    if (!sh || !buf || !len) return 0;
    return reinterpret_cast<UDPSocket*>(sh)->recvfrom(buf, len, out_src);
}

int32_t socket_close_udp(socket_impl_t sh) {
    if (!sh) return SOCK_ERR_INVAL;
    return reinterpret_cast<UDPSocket*>(sh)->close();
}

int32_t socket_setopt_udp(socket_impl_t sh, int32_t opt, const void* value, uint32_t len) {
    if (!sh) return SOCK_ERR_INVAL;
    return reinterpret_cast<UDPSocket*>(sh)->set_option(opt, value, len);
}

int32_t socket_getopt_udp(socket_impl_t sh, int32_t opt, void* value, uint32_t* len) {
    if (!sh) return SOCK_ERR_INVAL;
    return reinterpret_cast<UDPSocket*>(sh)->get_option(opt, value, len);
}

void socket_destroy_udp(socket_impl_t sh) {
    if (!sh) return;
    delete reinterpret_cast<UDPSocket*>(sh);
}

uint32_t socket_udp_input(ksocket_t* socket, uint8_t ifindex, uint8_t l3_id, ip_version_t ipver, const void* src_ip_addr, const void* dst_ip_addr, netpkt_t* pkt, uint16_t src_port, uint16_t dst_port) {
    if (!socket || !pkt) return 0;
    socket_impl_t sh = socket_core_impl(socket);
    if (!sh) return 0;
    return reinterpret_cast<UDPSocket*>(sh)->enqueue_datagram(ifindex, l3_id, ipver, src_ip_addr, dst_ip_addr, pkt, src_port, dst_port);
}

uint16_t socket_get_local_port_udp(socket_impl_t sh) {
    if (!sh) return 0;
    return reinterpret_cast<UDPSocket*>(sh)->get_local_port();
}

uint16_t socket_get_remote_port_udp(socket_impl_t sh) {
    if (!sh) return 0;
    return reinterpret_cast<UDPSocket*>(sh)->get_remote_port();
}

void socket_get_remote_ep_udp(socket_impl_t sh, net_l4_endpoint* out) {
    if (!sh || !out) return;
    *out = reinterpret_cast<UDPSocket*>(sh)->get_remote_ep();
}

uint8_t socket_get_protocol_udp(socket_impl_t sh) {
    if (!sh) return 0xFF;
    return reinterpret_cast<UDPSocket*>(sh)->get_protocol();
}

uint8_t socket_get_role_udp(socket_impl_t sh) {
    if (!sh) return 0xFF;
    return reinterpret_cast<UDPSocket*>(sh)->get_role();
}

bool socket_is_bound_udp(socket_impl_t sh) {
    if (!sh) return false;
    return reinterpret_cast<UDPSocket*>(sh)->is_bound();
}

bool socket_is_connected_udp(socket_impl_t sh) {
    if (!sh) return false;
    return reinterpret_cast<UDPSocket*>(sh)->is_connected();
}

}
