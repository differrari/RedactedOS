#include "networking/transport_layer/socket_tcp.hpp"
#include "networking/transport_layer/socket.hpp"
#include "csocket_tcp.h"

extern "C" {

socket_impl_t socket_tcp_create(ksocket_t* owner, uint8_t role, uint32_t pid, const SocketExtraOptions* extra) {
    if (!owner) return nullptr;
    return reinterpret_cast<socket_impl_t>(new TCPSocket(owner, role, pid, extra));
}

int32_t socket_bind_tcp(socket_impl_t sh, const SockBindSpec* spec, uint16_t port) {
    if (!sh || !spec) return SOCK_ERR_INVAL;
    return reinterpret_cast<TCPSocket*>(sh)->bind(*spec, port);
}

int32_t socket_listen_tcp(socket_impl_t sh, int32_t backlog) {
    if (!sh) return SOCK_ERR_INVAL;
    return reinterpret_cast<TCPSocket*>(sh)->listen(backlog);
}

ksocket_t* socket_accept_tcp(socket_impl_t sh) {
    if (!sh) return nullptr;
    return reinterpret_cast<TCPSocket*>(sh)->accept();
}

int32_t socket_connect_tcp(socket_impl_t sh, const net_l4_endpoint* dst) {
    if (!sh || !dst) return SOCK_ERR_INVAL;
    return reinterpret_cast<TCPSocket*>(sh)->connect(dst);
}

int64_t socket_send_tcp(socket_impl_t sh, const void* buf, uint64_t len) {
    if (!sh) return SOCK_ERR_INVAL;
    return reinterpret_cast<TCPSocket*>(sh)->send(buf, len);
}

int64_t socket_recv_tcp(socket_impl_t sh, void* buf, uint64_t len) {
    if (!sh) return SOCK_ERR_INVAL;
    return reinterpret_cast<TCPSocket*>(sh)->recv(buf, len);
}

int32_t socket_close_tcp(socket_impl_t sh) {
    if (!sh) return SOCK_ERR_INVAL;
    return reinterpret_cast<TCPSocket*>(sh)->close();
}

int32_t socket_setopt_tcp(socket_impl_t sh, int32_t opt, const void* value, uint32_t len) {
    if (!sh) return SOCK_ERR_INVAL;
    return reinterpret_cast<TCPSocket*>(sh)->set_option(opt, value, len);
}

int32_t socket_getopt_tcp(socket_impl_t sh, int32_t opt, void* value, uint32_t* len) {
    if (!sh) return SOCK_ERR_INVAL;
    return reinterpret_cast<TCPSocket*>(sh)->get_option(opt, value, len);
}

void socket_destroy_tcp(socket_impl_t sh) {
    if (!sh) return;
    delete reinterpret_cast<TCPSocket*>(sh);
}

const SocketExtraOptions* socket_tcp_extra_options(socket_impl_t sh) {
    if (!sh) return nullptr;
    return reinterpret_cast<TCPSocket*>(sh)->get_extra_options();
}

uint32_t tcp_accept_enqueue(ksocket_t* listener, uint8_t ifindex, ip_version_t ipver, const void* src_ip_addr, const void* dst_ip_addr, uint16_t src_port, uint16_t dst_port) {
    if (!listener) return 0;
    socket_impl_t sh = socket_core_impl(listener);
    if (!sh) return 0;
    return reinterpret_cast<TCPSocket*>(sh)->queue_accepted_child(ifindex, ipver, src_ip_addr, dst_ip_addr, src_port, dst_port);
}

uint16_t socket_get_local_port_tcp(socket_impl_t sh) {
    if (!sh) return 0;
    return reinterpret_cast<TCPSocket*>(sh)->get_local_port();
}

uint16_t socket_get_remote_port_tcp(socket_impl_t sh) {
    if (!sh) return 0;
    return reinterpret_cast<TCPSocket*>(sh)->get_remote_port();
}

void socket_get_remote_ep_tcp(socket_impl_t sh, net_l4_endpoint* out) {
    if (!sh || !out) return;
    *out = reinterpret_cast<TCPSocket*>(sh)->get_remote_ep();
}

uint8_t socket_get_protocol_tcp(socket_impl_t sh) {
    if (!sh) return 0xFF;
    return reinterpret_cast<TCPSocket*>(sh)->get_protocol();
}

uint8_t socket_get_role_tcp(socket_impl_t sh) {
    if (!sh) return 0xFF;
    return reinterpret_cast<TCPSocket*>(sh)->get_role();
}

bool socket_is_bound_tcp(socket_impl_t sh) {
    if (!sh) return false;
    return reinterpret_cast<TCPSocket*>(sh)->is_bound();
}

bool socket_is_connected_tcp(socket_impl_t sh) {
    if (!sh) return false;
    return reinterpret_cast<TCPSocket*>(sh)->is_connected();
}

}
