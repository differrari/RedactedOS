#include "socket_core.h"
#include "socket_bind.h"
#include "exceptions/irq.h"
#include "std/memory.h"
#include "alloc/allocate.h"

struct ksocket {
    uint32_t id;
    uint32_t generation;
    uint16_t pid;
    Socket_Role role;
    protocol_t protocol;
    socket_impl_t impl;
    socket_impl_destroy_fn destroy;
    socket_impl_close_fn close;
    socket_impl_setopt_fn setopt;
    socket_impl_getopt_fn getopt;
    int32_t refs;
    bool closing;
    bool visible;
};

static ksocket_t* sockets[SOCKET_MAX_OPEN];
static uint32_t generations[SOCKET_MAX_OPEN];

bool socket_core_alloc(Socket_Role role, protocol_t protocol, uint16_t pid, ksocket_t** out_socket) {
    if (!out_socket) return false;
    if (protocol == PROTO_NONE) return false;

    ksocket_t* socket = (ksocket_t*)zalloc(sizeof(ksocket_t));
    if (!socket) return false;

    irq_flags_t irq = irq_save_disable(); //TODO lock

    uint32_t id = SOCKET_MAX_OPEN;
    for (uint32_t i = 1; i < SOCKET_MAX_OPEN; ++i) {
        if (!sockets[i]) {
            id = i;
            break;
        }
    }

    if (id == SOCKET_MAX_OPEN) {
        irq_restore(irq);
        release(socket);
        return false;
    }

    uint32_t gen = generations[id] + 1;
    if (!gen) gen = 1;
    generations[id] = gen;

    socket->id = id;
    socket->generation = gen;
    socket->pid = pid;
    socket->role = role;
    socket->protocol = protocol;
    socket->refs = 1;
    sockets[id] = socket;

    irq_restore(irq);

    *out_socket = socket;
    return true;
}

bool socket_core_attach_impl(ksocket_t* socket, socket_impl_t impl, socket_impl_destroy_fn destroy, socket_impl_close_fn close, socket_impl_setopt_fn setopt, socket_impl_getopt_fn getopt, SocketHandle* out_handle) {
    if (!socket || !impl || !destroy || !close) return false;
    irq_flags_t irq = irq_save_disable(); //TODO lock
    if (socket->closing || socket->visible || socket->impl) {
        irq_restore(irq);
        return false;
    }

    socket->impl = impl;
    socket->destroy = destroy;
    socket->close = close;
    socket->setopt = setopt;
    socket->getopt = getopt;
    socket->visible = true;
    irq_restore(irq);

    if (out_handle) socket_core_export_handle(socket, out_handle);
    return true;
}

ksocket_t* socket_core_get(const SocketHandle* handle, uint16_t pid) {
    if (!handle || handle->protocol == PROTO_NONE) return NULL;
    if (handle->id >= SOCKET_MAX_OPEN) return NULL;

    irq_flags_t irq = irq_save_disable(); //TODO lock
    ksocket_t* socket = sockets[handle->id];
    if (!socket || !socket->visible || socket->closing || socket->generation != handle->generation || (pid && socket->pid != pid) || socket->protocol != handle->protocol) {
        irq_restore(irq);
        return NULL;
    }
    socket->refs++;
    irq_restore(irq);
    return socket;
}

void socket_core_ref(ksocket_t* socket) {
    if (!socket) return;
    irq_flags_t irq = irq_save_disable(); //TODO lock
    socket->refs++;
    irq_restore(irq);
}

void socket_core_put(ksocket_t* socket) {
    if (!socket) return;

    bool do_destroy = false;
    irq_flags_t irq = irq_save_disable(); 
    if (socket->refs > 0) socket->refs--;
    if (socket->refs == 0 && socket->closing) do_destroy = true;
    irq_restore(irq);

    if (!do_destroy) return;

    socket_impl_t impl = socket->impl;
    socket_impl_destroy_fn destroy = socket->destroy;
    socket->impl = NULL;
    socket->destroy = NULL;
    socket->close = NULL;
    socket->setopt = NULL;
    socket->getopt = NULL;

    if (destroy && impl) destroy(impl);
    release(socket);
}

int32_t socket_core_close_socket(ksocket_t* socket) {
    if (!socket) return SOCK_ERR_INVAL;

    bool first_close = false;
    irq_flags_t irq = irq_save_disable(); 
    if (!socket->closing) {
        socket->closing = true;
        socket->visible = false;
        if (socket->id < SOCKET_MAX_OPEN && sockets[socket->id] == socket) sockets[socket->id] = NULL;
        first_close = true;
    }
    irq_restore(irq);

    if (!first_close) return SOCK_OK;
    socket_bind_remove_socket(socket);

    int32_t ret = SOCK_OK;
    if (socket->close && socket->impl) ret = socket->close(socket->impl);

    socket_core_put(socket);
    return ret;
}

int32_t socket_core_close_handle(SocketHandle* handle, uint16_t pid) {
    ksocket_t* socket = socket_core_get(handle, pid);
    if (!socket) return SOCK_ERR_INVAL;
    int32_t ret = socket_core_close_socket(socket);
    socket_core_put(socket);
    if (handle) {
        handle->id = 0;
        handle->generation = 0;
        handle->protocol = PROTO_NONE;
        handle->connection = (net_l4_endpoint){};
    }
    return ret;
}

int32_t socket_core_set_option(ksocket_t* socket, int32_t opt, const void* value, uint32_t len) {
    if (!socket || !value || !len) return SOCK_ERR_INVAL;
    if (!socket->setopt || !socket->impl) return SOCK_ERR_PROTO;
    return socket->setopt(socket->impl, opt, value, len);
}

int32_t socket_core_get_option(ksocket_t* socket, int32_t opt, void* value, uint32_t* len) {
    if (!socket || !value || !len) return SOCK_ERR_INVAL;
    if (!socket->getopt || !socket->impl) return SOCK_ERR_PROTO;
    return socket->getopt(socket->impl, opt, value, len);
}

socket_impl_t socket_core_impl(ksocket_t* socket) {
    return socket ? socket->impl : NULL;
}

protocol_t socket_core_protocol(const ksocket_t* socket) {
    return socket ? socket->protocol : PROTO_NONE;
}

Socket_Role socket_core_role(const ksocket_t* socket) {
    return socket ? socket->role : SOCKET_CLIENT;
}

uint16_t socket_core_pid(const ksocket_t* socket) {
    return socket ? socket->pid : 0;
}

uint32_t socket_core_id(const ksocket_t* socket) {
    return socket ? socket->id : 0;
}

uint32_t socket_core_generation(const ksocket_t* socket) {
    return socket ? socket->generation : 0;
}

bool socket_core_is_closing(const ksocket_t* socket) {
    return !socket || socket->closing;
}

void socket_core_export_handle(const ksocket_t* socket, SocketHandle* out_handle) {
    if (!socket || !out_handle) return;
    out_handle->id = socket->id;
    out_handle->generation = socket->generation;
    out_handle->protocol = socket->protocol;
    out_handle->connection = (net_l4_endpoint){};
}
