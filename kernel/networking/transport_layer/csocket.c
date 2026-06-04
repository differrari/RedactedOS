#include "csocket.h"
#include "csocket_tcp.h"
#include "csocket_udp.h"
#include "socket_core.h"
#include "process/scheduler.h"
#include "std/memory.h"

bool create_socket(Socket_Role role, protocol_t protocol, const SocketExtraOptions* extra, SocketHandle *out_handle){
    if (!out_handle) return false;
    memset(out_handle, 0, sizeof(*out_handle));

    if (protocol == PROTO_NONE) {
        //TODO PROTO_NONE for RAW SET or L2
        return false;
    }
    if (protocol != PROTO_TCP && protocol != PROTO_UDP) return false;

    uint16_t pid = get_current_proc_pid();
    ksocket_t* socket = NULL;
    if (!socket_core_alloc(role, protocol, pid, &socket)) return false;

    socket_impl_t impl = NULL;
    socket_impl_destroy_fn destroy = NULL;
    socket_impl_close_fn close = NULL;
    socket_impl_setopt_fn setopt = NULL;
    socket_impl_getopt_fn getopt = NULL;

    if (protocol == PROTO_UDP) {
        impl = udp_socket_create(socket, role, pid, extra);
        destroy = socket_destroy_udp;
        close = socket_close_udp;
        setopt = socket_setopt_udp;
        getopt = socket_getopt_udp;
    } else if (protocol == PROTO_TCP) {
        impl = socket_tcp_create(socket, role, pid, extra);
        destroy = socket_destroy_tcp;
        close = socket_close_tcp;
        setopt = socket_setopt_tcp;
        getopt = socket_getopt_tcp;
    }
    if (!impl) {
        socket_core_close_socket(socket);
        return false;
    }

    if (!socket_core_attach_impl(socket, impl, destroy, close, setopt, getopt, out_handle)) {
        destroy(impl);
        socket_core_close_socket(socket);
        return false;
    }
    return true;
}

int32_t bind_socket(SocketHandle *handle, uint16_t port, ip_version_t ip_version){
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return SOCK_ERR_INVAL;

    SockBindSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = BIND_IP;
    spec.ver = ip_version;
    if (handle) {
        if (ip_version == IP_VER4) memcpy(spec.ip, handle->connection.ip, 4);
        else if (ip_version == IP_VER6) memcpy(spec.ip, handle->connection.ip, 16);
        else spec.kind = BIND_ANY;
    }

    int32_t res = SOCK_ERR_PROTO;
    if (socket_core_protocol(socket) == PROTO_TCP) res = socket_bind_tcp(socket_core_impl(socket), &spec, port);
    else if (socket_core_protocol(socket) == PROTO_UDP) res = socket_bind_udp(socket_core_impl(socket), &spec, port);

    socket_core_put(socket);
    return res;
}

int32_t bind_socket_spec(SocketHandle* handle, const SockBindSpec* spec, uint16_t port){
    if (!spec) return SOCK_ERR_INVAL;
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return SOCK_ERR_INVAL;

    int32_t res = SOCK_ERR_PROTO;
    if (socket_core_protocol(socket) == PROTO_TCP) res = socket_bind_tcp(socket_core_impl(socket), spec, port);
    else if (socket_core_protocol(socket) == PROTO_UDP) res = socket_bind_udp(socket_core_impl(socket), spec, port);

    socket_core_put(socket);
    return res;
}

int32_t connect_socket(SocketHandle *handle, const net_l4_endpoint* dst){
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return SOCK_ERR_INVAL;

    int32_t res = SOCK_ERR_PROTO;
    if (socket_core_protocol(socket) == PROTO_TCP) {
        res = socket_connect_tcp(socket_core_impl(socket), dst);
        if (res == SOCK_OK && handle) socket_get_remote_ep_tcp(socket_core_impl(socket), &handle->connection);
    }

    socket_core_put(socket);
    return res;
}

int64_t send_on_socket(SocketHandle *handle, const void* buf, uint64_t len){
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return SOCK_ERR_INVAL;

    int64_t res = SOCK_ERR_PROTO;
    if (socket_core_protocol(socket) == PROTO_TCP) res = socket_send_tcp(socket_core_impl(socket), buf, len);
    else if (socket_core_protocol(socket) == PROTO_UDP) res = socket_sendto_udp(socket_core_impl(socket), NULL, buf, len);

    socket_core_put(socket);
    return res;
}

int64_t send_to_socket(SocketHandle *handle, const net_l4_endpoint* dst, const void* buf, uint64_t len){
    if (!dst) return SOCK_ERR_INVAL;
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return SOCK_ERR_INVAL;

    int64_t res = SOCK_ERR_PROTO;
    if (socket_core_protocol(socket) == PROTO_UDP) res = socket_sendto_udp(socket_core_impl(socket), dst, buf, len);

    socket_core_put(socket);
    return res;
}

int64_t receive_from_socket(SocketHandle *handle, void* buf, uint64_t len, net_l4_endpoint* out_src){
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return SOCK_ERR_INVAL;

    int64_t res = SOCK_ERR_PROTO;
    if (socket_core_protocol(socket) == PROTO_TCP) res = socket_recv_tcp(socket_core_impl(socket), buf, len);
    else if (socket_core_protocol(socket) == PROTO_UDP) res = socket_recvfrom_udp(socket_core_impl(socket), buf, len, out_src);

    socket_core_put(socket);
    return res;
}

int32_t set_socket_option(SocketHandle *handle, int32_t opt, const void* value, uint32_t len){
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return SOCK_ERR_INVAL;
    int32_t res = socket_core_set_option(socket, opt, value, len);
    socket_core_put(socket);
    return res;
}

int32_t get_socket_option(SocketHandle *handle, int32_t opt, void* value, uint32_t* len){
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return SOCK_ERR_INVAL;
    int32_t res = socket_core_get_option(socket, opt, value, len);
    socket_core_put(socket);
    return res;
}

uint16_t get_socket_local_port(SocketHandle* handle){
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return 0;

    uint16_t port = 0;
    if (socket_core_protocol(socket) == PROTO_TCP) port = socket_get_local_port_tcp(socket_core_impl(socket));
    else if (socket_core_protocol(socket) == PROTO_UDP) port = socket_get_local_port_udp(socket_core_impl(socket));

    socket_core_put(socket);
    return port;
}

bool get_socket_remote_endpoint(SocketHandle *handle, net_l4_endpoint* out){
    if (!out) return false;
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return false;

    bool ok = true;
    if (socket_core_protocol(socket) == PROTO_TCP) socket_get_remote_ep_tcp(socket_core_impl(socket), out);
    else if (socket_core_protocol(socket) == PROTO_UDP) socket_get_remote_ep_udp(socket_core_impl(socket), out);
    else ok = false;

    socket_core_put(socket);
    return ok;
}

int32_t close_socket(SocketHandle *handle){
    return socket_core_close_handle(handle, get_current_proc_pid());
}

int32_t listen_on(SocketHandle *handle, int32_t backlog){
    ksocket_t* socket = socket_core_get(handle, get_current_proc_pid());
    if (!socket) return SOCK_ERR_INVAL;

    int32_t res = SOCK_ERR_PROTO;
    if (socket_core_protocol(socket) == PROTO_TCP) res = socket_listen_tcp(socket_core_impl(socket), backlog);

    socket_core_put(socket);
    return res;
}

bool accept_on_socket(SocketHandle *handle, SocketHandle* out_child) {
    if (!out_child) return false;
    memset(out_child, 0, sizeof(*out_child));
    ksocket_t* listener = socket_core_get(handle, get_current_proc_pid());
    if (!listener) return false;
    if (socket_core_protocol(listener) != PROTO_TCP) {
        socket_core_put(listener);
        return false;
    }

    ksocket_t* child = socket_accept_tcp(socket_core_impl(listener));
    if (!child) {
        socket_core_put(listener);
        return false;
    }

    socket_core_export_handle(child, out_child);
    socket_get_remote_ep_tcp(socket_core_impl(child), &out_child->connection);
    socket_core_put(child);
    socket_core_put(listener);
    return true;
}