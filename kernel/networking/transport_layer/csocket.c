#include "csocket.h"
#include "csocket_raw.h"
#include "csocket_special.h"
#include "csocket_tcp.h"
#include "csocket_udp.h"
#include "socket_core.h"
#include "process/scheduler.h"
#include "std/memory.h"
#include "memory/page_allocator.h"
#include "console/kio.h"
#include "data/struct/hashmap.h"
#include "alloc/allocate.h"

socket_handle_t create_socket(protocol_t protocol, const SocketOptions* extra){
    SocketOptions default_extra = {};
    if (!extra) extra = &default_extra;

    SocketSpecialKind special_kind = extra->special_kind;
    bool normal_socket = special_kind == SOCKET_SPECIAL_NONE && (protocol == PROTO_TCP || protocol == PROTO_UDP);
    bool raw_socket = special_kind == SOCKET_SPECIAL_RAW && (protocol == PROTO_ICMP || protocol == PROTO_ICMPV6 || protocol == PROTO_IGMP);
    bool ctrl_socket = special_kind == SOCKET_SPECIAL_CTRL && protocol == PROTO_NONE;
    bool packet_socket = special_kind == SOCKET_SPECIAL_PACKET && protocol == PROTO_NONE;
    if (!normal_socket && !raw_socket && !ctrl_socket && !packet_socket) return 0;

    uint16_t pid = get_current_proc_pid();
    ksocket_t* socket = NULL;
    if (!socket_core_alloc(protocol, special_kind, pid, &socket)) return 0;

    socket_impl_t impl = NULL;
    socket_impl_destroy_fn destroy = NULL;
    socket_impl_close_fn close = NULL;
    socket_impl_setopt_fn setopt = NULL;
    socket_impl_getopt_fn getopt = NULL;

    if (protocol == PROTO_UDP) {
        impl = udp_socket_create(socket, extra);
        destroy = socket_destroy_udp;
        close = socket_close_udp;
        setopt = socket_setopt_udp;
        getopt = socket_getopt_udp;
    } else if (protocol == PROTO_TCP) {
        impl = socket_tcp_create(socket, extra);
        destroy = socket_destroy_tcp;
        close = socket_close_tcp;
        setopt = socket_setopt_tcp;
        getopt = socket_getopt_tcp;
    } else if (special_kind == SOCKET_SPECIAL_RAW) {
        impl = socket_raw_create(socket, extra);
        destroy = socket_destroy_raw;
        close = socket_close_raw;
        setopt = socket_setopt_raw;
        getopt = socket_getopt_raw;
    } else {
        impl = socket_special_create(socket, extra);
        destroy = socket_destroy_special;
        close = socket_close_special;
        setopt = socket_setopt_special;
        getopt = socket_getopt_special;
    }
    if (!impl) {
        socket_core_close_socket(socket);
        return 0;
    }

    if (!socket_core_attach_impl(socket, impl, destroy, close, setopt, getopt)) {
        destroy(impl);
        socket_core_close_socket(socket);
        return 0;
    }
    return socket_core_export_handle(socket);
}

int32_t bind_socket(socket_handle_t handle, const SockBindSpec *spec_in, uint16_t port){
    SockBindSpec spec;
    memset(&spec, 0, sizeof(spec));
    if (spec_in) spec = *spec_in;
    else spec.kind = BIND_ANY;

    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return SOCK_ERR_INVAL;

    int32_t res = SOCK_ERR_PROTO;
    if (socket_core_special_kind(socket) == SOCKET_SPECIAL_RAW) res = socket_bind_raw(socket_core_impl(socket), &spec);
    else if (socket_core_special_kind(socket) != SOCKET_SPECIAL_NONE) res = SOCK_ERR_UNSUP;
    else if (socket_core_protocol(socket) == PROTO_TCP) res = socket_bind_tcp(socket_core_impl(socket), &spec, port);
    else if (socket_core_protocol(socket) == PROTO_UDP) res = socket_bind_udp(socket_core_impl(socket), &spec, port);

    socket_core_put(socket);

    return res;
}

int32_t connect_socket(socket_handle_t handle, const net_l4_endpoint* dst){
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return SOCK_ERR_INVAL;

    int32_t res = SOCK_ERR_PROTO;
    if (socket_core_special_kind(socket) == SOCKET_SPECIAL_RAW) res = socket_connect_raw(socket_core_impl(socket), dst);
    else if (socket_core_special_kind(socket) != SOCKET_SPECIAL_NONE) res = SOCK_ERR_UNSUP;
    else if (socket_core_protocol(socket) == PROTO_TCP) {
        res = socket_connect_tcp(socket_core_impl(socket), dst);
    } else if (socket_core_protocol(socket) == PROTO_UDP) res = socket_connect_udp(socket_core_impl(socket), dst);

    socket_core_put(socket);
    return res;
}

int64_t send_on_socket(socket_handle_t handle, const void* buf, uint64_t len){
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return SOCK_ERR_INVAL;

    int64_t res = SOCK_ERR_PROTO;
    if (socket_core_special_kind(socket) == SOCKET_SPECIAL_RAW) res = socket_send_raw(socket_core_impl(socket), buf, len);
    else if (socket_core_special_kind(socket) != SOCKET_SPECIAL_NONE) res = socket_send_special(socket_core_impl(socket), buf, len);
    else if (socket_core_protocol(socket) == PROTO_TCP) res = socket_send_tcp(socket_core_impl(socket), buf, len);
    else if (socket_core_protocol(socket) == PROTO_UDP) res = socket_sendto_udp(socket_core_impl(socket), NULL, buf, len);

    socket_core_put(socket);
    return res;
}

int64_t send_to_socket(socket_handle_t handle, const net_l4_endpoint* dst, const void* buf, uint64_t len){
    if (!dst) return SOCK_ERR_INVAL;
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return SOCK_ERR_INVAL;

    int64_t res = SOCK_ERR_PROTO;
    if (socket_core_special_kind(socket) == SOCKET_SPECIAL_RAW) res = socket_sendto_raw(socket_core_impl(socket), dst, buf, len);
    else if (socket_core_special_kind(socket) != SOCKET_SPECIAL_NONE) res = SOCK_ERR_UNSUP;
    else if (socket_core_protocol(socket) == PROTO_UDP) res = socket_sendto_udp(socket_core_impl(socket), dst, buf, len);

    socket_core_put(socket);
    return res;
}

int64_t receive_from_socket(socket_handle_t handle, void* buf, uint64_t len, net_l4_endpoint* out_src){
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return SOCK_ERR_INVAL;

    int64_t res = SOCK_ERR_PROTO;
    if (socket_core_special_kind(socket) == SOCKET_SPECIAL_RAW) res = socket_recv_raw(socket_core_impl(socket), buf, len, out_src);
    else if (socket_core_special_kind(socket) != SOCKET_SPECIAL_NONE) res = socket_recv_special(socket_core_impl(socket), buf, len);
    else if (socket_core_protocol(socket) == PROTO_TCP) res = socket_recv_tcp(socket_core_impl(socket), buf, len);
    else if (socket_core_protocol(socket) == PROTO_UDP) res = socket_recvfrom_udp(socket_core_impl(socket), buf, len, out_src);

    socket_core_put(socket);
    return res;
}

int32_t set_socket_option(socket_handle_t handle, int32_t opt, const void* value, uint32_t len){
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return SOCK_ERR_INVAL;
    int32_t res = socket_core_set_option(socket, opt, value, len);
    socket_core_put(socket);
    return res;
}

int32_t get_socket_option(socket_handle_t handle, int32_t opt, void* value, uint32_t* len){
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return SOCK_ERR_INVAL;
    int32_t res = socket_core_get_option(socket, opt, value, len);
    socket_core_put(socket);
    return res;
}

int32_t close_socket(socket_handle_t handle){
    return socket_core_close_handle(handle, get_current_proc_pid());
}

int32_t listen_on(socket_handle_t handle, int32_t backlog){
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return SOCK_ERR_INVAL;

    int32_t res = SOCK_ERR_PROTO;
    if (socket_core_special_kind(socket) != SOCKET_SPECIAL_NONE) res = SOCK_ERR_UNSUP;
    else if (socket_core_protocol(socket) == PROTO_TCP) res = socket_listen_tcp(socket_core_impl(socket), backlog);

    socket_core_put(socket);
    return res;
}

socket_handle_t accept_on_socket(socket_handle_t handle) {
    ksocket_t* listener = socket_core_get(handle, get_current_proc_pid());
    if (!listener) return 0;
    if (socket_core_special_kind(listener) != SOCKET_SPECIAL_NONE) {
        socket_core_put(listener);
        return 0;
    }
    if (socket_core_protocol(listener) != PROTO_TCP) {
        socket_core_put(listener);
        return 0;
    }

    ksocket_t* child = socket_accept_tcp(socket_core_impl(listener));
    if (!child) {
        socket_core_put(listener);
        return 0;
    }

    socket_handle_t child_handle = socket_core_export_handle(child);
    socket_core_put(child);
    socket_core_put(listener);
    return child_handle;
}