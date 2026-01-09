#include "networking/transport_layer/socket_tcp.hpp"
#include "networking/transport_layer/socket.hpp"
#include "csocket_tcp.h"

extern "C" {

socket_handle_t socket_tcp_create(uint8_t role, uint32_t pid, const SocketExtraOptions* extra) {
    return reinterpret_cast<socket_handle_t>(new TCPSocket(role, pid, extra));
}

int32_t socket_bind_tcp_ex(socket_handle_t sh, const SockBindSpec* spec, uint16_t port) {
    if (!sh || !spec) return SOCK_ERR_INVAL;
    return reinterpret_cast<TCPSocket*>(sh)->bind(*spec, port);
}

int32_t socket_listen_tcp(socket_handle_t sh, int32_t backlog) {
    if (!sh) return SOCK_ERR_INVAL;
    return reinterpret_cast<TCPSocket*>(sh)->listen(backlog);
}

socket_handle_t socket_accept_tcp(socket_handle_t sh) {
    if (!sh) return nullptr;
    TCPSocket* srv = reinterpret_cast<TCPSocket*>(sh);
    TCPSocket* client = srv->accept();
    return reinterpret_cast<socket_handle_t>(client);
}

int32_t socket_connect_tcp_ex(socket_handle_t sh, uint8_t dst_kind, const void* dst, uint16_t port) {
    if (!sh || !dst) return SOCK_ERR_INVAL;
    return reinterpret_cast<TCPSocket*>(sh)->connect(static_cast<SockDstKind>(dst_kind), dst, port);
}

int64_t socket_send_tcp(socket_handle_t sh, const void* buf, uint64_t len) {
    if (!sh) return SOCK_ERR_INVAL;
    return reinterpret_cast<TCPSocket*>(sh)->send(buf, len);
}

int64_t socket_recv_tcp(socket_handle_t sh, void* buf, uint64_t len) {
    if (!sh) return SOCK_ERR_INVAL;
    return reinterpret_cast<TCPSocket*>(sh)->recv(buf, len);
}

int32_t socket_close_tcp(socket_handle_t sh) {
    if (!sh) return SOCK_ERR_INVAL;
    return reinterpret_cast<TCPSocket*>(sh)->close();
}

void socket_destroy_tcp(socket_handle_t sh) {
    if (!sh) return;
    delete reinterpret_cast<TCPSocket*>(sh);
}

uint16_t socket_get_local_port_tcp(socket_handle_t sh) {
    if (!sh) return 0;
    return reinterpret_cast<TCPSocket*>(sh)->get_local_port();
}

uint16_t socket_get_remote_port_tcp(socket_handle_t sh) {
    if (!sh) return 0;
    return reinterpret_cast<TCPSocket*>(sh)->get_remote_port();
}

void socket_get_remote_ep_tcp(socket_handle_t sh, net_l4_endpoint* out) {
    if (!sh || !out) return;
    *out = reinterpret_cast<TCPSocket*>(sh)->get_remote_ep();
}

uint8_t socket_get_protocol_tcp(socket_handle_t sh) {
    if (!sh) return 0xFF;
    return reinterpret_cast<TCPSocket*>(sh)->get_protocol();
}

uint8_t socket_get_role_tcp(socket_handle_t sh) {
    if (!sh) return 0xFF;
    return reinterpret_cast<TCPSocket*>(sh)->get_role();
}

bool socket_is_bound_tcp(socket_handle_t sh) {
    if (!sh) return false;
    return reinterpret_cast<TCPSocket*>(sh)->is_bound();
}

bool socket_is_connected_tcp(socket_handle_t sh) {
    if (!sh) return false;
    return reinterpret_cast<TCPSocket*>(sh)->is_connected();
}

}
