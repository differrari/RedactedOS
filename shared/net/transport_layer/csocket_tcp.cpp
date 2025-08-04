#pragma once
#include "net/transport_layer/socket_tcp.hpp"
#include "net/transport_layer/socket.hpp"
#include "csocket_tcp.h"

extern "C" {

socket_handle_t socket_tcp_create(uint8_t role, uint32_t pid) {
    return reinterpret_cast<socket_handle_t>(new TCPSocket(role, pid));
}

int32_t socket_bind_tcp(socket_handle_t sh, uint16_t port) {
    return reinterpret_cast<Socket*>(sh)->bind(port);
}

int32_t socket_listen_tcp(socket_handle_t sh, int32_t backlog) {
    return reinterpret_cast<TCPSocket*>(sh)->listen(backlog);
}

socket_handle_t socket_accept_tcp(socket_handle_t sh) {
    TCPSocket* srv = reinterpret_cast<TCPSocket*>(sh);
    TCPSocket* client = srv->accept();
    return reinterpret_cast<socket_handle_t>(client);
}

int32_t socket_connect_tcp(socket_handle_t sh, uint32_t ip, uint16_t port) {
    return reinterpret_cast<TCPSocket*>(sh)->connect(ip, port);
}

int64_t socket_send_tcp(socket_handle_t sh, const void* buf, uint64_t len) {
    return reinterpret_cast<TCPSocket*>(sh)->send(buf, len);
}

int64_t socket_recv_tcp(socket_handle_t sh, void* buf, uint64_t len) {
    return reinterpret_cast<TCPSocket*>(sh)->recv(buf, len);
}

int32_t socket_close_tcp(socket_handle_t sh) {
    return reinterpret_cast<Socket*>(sh)->close();
}

void socket_destroy_tcp(socket_handle_t sh) {
    delete reinterpret_cast<Socket*>(sh);
}

uint16_t socket_get_local_port_tcp(socket_handle_t sh) {
    return reinterpret_cast<Socket*>(sh)->get_local_port();
}

uint32_t socket_get_remote_ip_tcp(socket_handle_t sh) {
    return reinterpret_cast<Socket*>(sh)->get_remote_ip();
}

uint16_t socket_get_remote_port_tcp(socket_handle_t sh) {
    return reinterpret_cast<Socket*>(sh)->get_remote_port();
}

uint8_t socket_get_protocol_tcp(socket_handle_t sh) {
    return reinterpret_cast<Socket*>(sh)->get_protocol();
}

uint8_t socket_get_role_tcp(socket_handle_t sh) {
    return reinterpret_cast<Socket*>(sh)->get_role();
}

bool socket_is_bound_tcp(socket_handle_t sh) {
    return reinterpret_cast<Socket*>(sh)->is_bound();
}

bool socket_is_connected_tcp(socket_handle_t sh) {
    return reinterpret_cast<Socket*>(sh)->is_connected();
}

}
