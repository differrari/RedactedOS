#include "networking/transport_layer/socket_udp.hpp"
#include "networking/transport_layer/socket.hpp"
#include "csocket_udp.h"

extern "C" socket_handle_t udp_socket_create(uint8_t role, uint32_t pid, const SocketExtraOptions* extra) {
    return reinterpret_cast<socket_handle_t>(new UDPSocket(role, pid, extra));
}

extern "C" int32_t socket_bind_udp_ex(socket_handle_t sh, const SockBindSpec* spec, uint16_t port) {
    if (!sh || !spec) return SOCK_ERR_INVAL;
    return reinterpret_cast<UDPSocket*>(sh)->bind(*spec, port);
}

extern "C" int64_t socket_sendto_udp_ex(socket_handle_t sh, uint8_t dst_kind, const void* dst, uint16_t port, const void* buf, uint64_t len) {
    if (!sh || !dst || !buf || !len) return SOCK_ERR_INVAL;
    return reinterpret_cast<UDPSocket*>(sh)->sendto(static_cast<SockDstKind>(dst_kind), dst, port, buf, len);
}

extern "C" int64_t socket_recvfrom_udp_ex(socket_handle_t sh, void* buf, uint64_t len, net_l4_endpoint* out_src) {
    if (!sh || !buf || !len) return 0;
    return reinterpret_cast<UDPSocket*>(sh)->recvfrom(buf, len, out_src);
}

extern "C" int32_t socket_close_udp(socket_handle_t sh) {
    if (!sh) return SOCK_ERR_INVAL;
    return reinterpret_cast<UDPSocket*>(sh)->close();
}

extern "C" void socket_destroy_udp(socket_handle_t sh) {
    if (!sh) return;
    delete reinterpret_cast<UDPSocket*>(sh);
}

extern "C" uint16_t socket_get_local_port_udp(socket_handle_t sh) {
    if (!sh) return 0;
    return reinterpret_cast<UDPSocket*>(sh)->get_local_port();
}

extern "C" uint16_t socket_get_remote_port_udp(socket_handle_t sh) {
    if (!sh) return 0;
    return reinterpret_cast<UDPSocket*>(sh)->get_remote_port();
}

extern "C" void socket_get_remote_ep_udp(socket_handle_t sh, net_l4_endpoint* out) {
    if (!sh || !out) return;
    *out = reinterpret_cast<UDPSocket*>(sh)->get_remote_ep();
}

extern "C" uint8_t socket_get_protocol_udp(socket_handle_t sh) {
    if (!sh) return 0xFF;
    return reinterpret_cast<UDPSocket*>(sh)->get_protocol();
}

extern "C" uint8_t socket_get_role_udp(socket_handle_t sh) {
    if (!sh) return 0xFF;
    return reinterpret_cast<UDPSocket*>(sh)->get_role();
}

extern "C" bool socket_is_bound_udp(socket_handle_t sh) {
    if (!sh) return false;
    return reinterpret_cast<UDPSocket*>(sh)->is_bound();
}

extern "C" bool socket_is_connected_udp(socket_handle_t sh) {
    if (!sh) return false;
    return reinterpret_cast<UDPSocket*>(sh)->is_connected();
}
