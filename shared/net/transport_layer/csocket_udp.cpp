#pragma once
#include "net/transport_layer/socket_udp.hpp"
#include "net/transport_layer/socket.hpp"
#include "csocket_udp.h"

extern "C" socket_handle_t udp_socket_create(uint8_t role, uint32_t pid) {
    return reinterpret_cast<socket_handle_t>(new UDPSocket(role, pid));
}

extern "C" int32_t socket_bind_udp(socket_handle_t sh, uint16_t port) {
    return reinterpret_cast<Socket*>(sh)->bind(port);
}

extern "C" int64_t socket_sendto_udp(socket_handle_t sh,
                                    uint32_t ip, uint16_t port,
                                    const void* buf, uint64_t len) {
    auto sock = reinterpret_cast<UDPSocket*>(sh);
    return sock->sendto(ip, port, buf, len);
}

extern "C" int64_t socket_recvfrom_udp(socket_handle_t sh,
                                    void* buf, uint64_t len,
                                    uint32_t* out_ip, uint16_t* out_port) {
    auto sock = reinterpret_cast<UDPSocket*>(sh);
    return sock->recvfrom(buf, len, out_ip, out_port);
}

extern "C" int32_t socket_close_udp(socket_handle_t sh) {
    return reinterpret_cast<UDPSocket*>(sh)->close();
}

extern "C" void socket_destroy_udp(socket_handle_t sh) {
    delete reinterpret_cast<UDPSocket*>(sh);
}

extern "C" uint16_t socket_get_local_port_udp(socket_handle_t sh) {
    return reinterpret_cast<UDPSocket*>(sh)->get_local_port();
}

extern "C" uint16_t socket_get_remote_port_udp(socket_handle_t sh) {
    return reinterpret_cast<UDPSocket*>(sh)->get_remote_port();
}

extern "C" uint32_t socket_get_remote_ip_udp(socket_handle_t sh) {
    return reinterpret_cast<UDPSocket*>(sh)->get_remote_ip();
}

extern "C" uint8_t socket_get_protocol_udp(socket_handle_t sh) {
    return reinterpret_cast<UDPSocket*>(sh)->get_protocol();
}

extern "C" uint8_t socket_get_role_udp(socket_handle_t sh) {
    return reinterpret_cast<UDPSocket*>(sh)->get_role();
}

extern "C" bool socket_is_bound_udp(socket_handle_t sh) {
    return reinterpret_cast<UDPSocket*>(sh)->is_bound();
}

extern "C" bool socket_is_connected_udp(socket_handle_t sh) {
    return reinterpret_cast<UDPSocket*>(sh)->is_connected();
}
