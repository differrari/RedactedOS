#include "socket_core.h"
#include "socket_bind.h"
#include "exceptions/irq.h"
#include "std/memory.h"
#include "alloc/allocate.h"

struct ksocket {
    uint32_t id;
    uint64_t generation;
    uint16_t pid;
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
static uint64_t generations[SOCKET_MAX_OPEN];

bool socket_core_alloc(protocol_t protocol, uint16_t pid, ksocket_t** out_socket) {
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

    uint64_t gen = generations[id] + 1;
    if (!gen) gen = 1;
    generations[id] = gen;

    socket->id = id;
    socket->generation = gen;
    socket->pid = pid;
    socket->protocol = protocol;
    socket->refs = 1;
    sockets[id] = socket;

    irq_restore(irq);

    *out_socket = socket;
    return true;
}

bool socket_core_attach_impl(ksocket_t* socket, socket_impl_t impl, socket_impl_destroy_fn destroy, socket_impl_close_fn close, socket_impl_setopt_fn setopt, socket_impl_getopt_fn getopt) {
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

    return true;
}

ksocket_t* socket_core_get(socket_handle_t handle, uint16_t pid) {
    if (!handle) return NULL;

    uint64_t index_mask = ((uint64_t)1 << SOCKET_HANDLE_INDEX_BITS) - 1;
    uint32_t id = (uint32_t)(handle & index_mask);
    uint64_t generation = handle >> SOCKET_HANDLE_INDEX_BITS;
    if (!id || id >= SOCKET_MAX_OPEN || !generation) return NULL;

    irq_flags_t irq = irq_save_disable(); //TODO lock
    ksocket_t* socket = sockets[id];
    if (!socket || !socket->visible || socket->closing || socket->generation != generation || (pid && socket->pid != pid)) {
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
    int32_t ret = SOCK_OK;
    if (socket->close && socket->impl) ret = socket->close(socket->impl);

    socket_core_put(socket);
    return ret;
}

int32_t socket_core_close_handle(socket_handle_t handle, uint16_t pid) {
    ksocket_t* socket = socket_core_get(handle, pid);
    if (!socket) return SOCK_ERR_INVAL;
    int32_t ret = socket_core_close_socket(socket);
    socket_core_put(socket);
    return ret;
}

int32_t socket_core_set_option(ksocket_t* socket, int32_t opt, const void* value, uint32_t len) {
    if (!socket) return SOCK_ERR_INVAL;
    if (!value && len) return SOCK_ERR_INVAL;
    if (!socket->setopt || !socket->impl) return SOCK_ERR_PROTO;
    return socket->setopt(socket->impl, opt, value, len);
}

int32_t socket_core_get_option(ksocket_t* socket, int32_t opt, void* value, uint32_t* len) {
    if (!socket || !len) return SOCK_ERR_INVAL;
    if (!socket->getopt || !socket->impl) return SOCK_ERR_PROTO;

    uint32_t v = 0;
    if (opt == SOCK_GET_PROTOCOL) v = socket->protocol;
    else if (opt == SOCK_GET_OWNER_PID) v = socket->pid;
    else return socket->getopt(socket->impl, opt, value, len);

    if (!value) {
        *len = sizeof(uint32_t);
        return SOCK_OK;
    }
    if (*len < sizeof(uint32_t)) return SOCK_ERR_INVAL;
    memcpy(value, &v, sizeof(v));
    *len = sizeof(uint32_t);
    return SOCK_OK;
}

int32_t socket_common_options_set(SocketOptions* opts, int32_t opt, const void* value, uint32_t len) {
    if (!opts) return SOCK_ERR_INVAL;

    uint32_t v = 1;
    if (value) {
        if (len != sizeof(uint32_t)) return SOCK_ERR_INVAL;
        memcpy(&v, value, sizeof(v));
    } else if (len || (opt != SOCK_OPT_DONTFRAG && opt != SOCK_OPT_BROADCAST_ALLOWED)) return SOCK_ERR_INVAL;

    switch ((uint32_t)opt) {
        case SOCK_OPT_RECV_TIMEOUT:
            opts->recv_timeout_ms = v;
            if (v) opts->flags |= SOCK_OPT_RECV_TIMEOUT;
            else opts->flags &= ~SOCK_OPT_RECV_TIMEOUT;
            return SOCK_OK;
        case SOCK_OPT_SEND_TIMEOUT:
            opts->send_timeout_ms = v;
            if (v) opts->flags |= SOCK_OPT_SEND_TIMEOUT;
            else opts->flags &= ~SOCK_OPT_SEND_TIMEOUT;
            return SOCK_OK;
        case SOCK_OPT_BUF_SIZE:
            if (!v) return SOCK_ERR_INVAL;
            opts->buf_size = v;
            opts->flags |= SOCK_OPT_BUF_SIZE;
            return SOCK_OK;
        case SOCK_OPT_DEBUG:
            if (v > SOCK_DBG_ALL) return SOCK_ERR_INVAL;
            opts->debug_level = (SockDebugLevel)v;
            if (v) opts->flags |= SOCK_OPT_DEBUG;
            else opts->flags &= ~SOCK_OPT_DEBUG;
            return SOCK_OK;
        case SOCK_OPT_DONTFRAG:
            if (v) opts->flags |= SOCK_OPT_DONTFRAG;
            else opts->flags &= ~SOCK_OPT_DONTFRAG;
            return SOCK_OK;
        case SOCK_OPT_BROADCAST_ALLOWED:
            if (v) opts->flags |= SOCK_OPT_BROADCAST_ALLOWED;
            else opts->flags &= ~SOCK_OPT_BROADCAST_ALLOWED;
            return SOCK_OK;
        case SOCK_OPT_TTL:
            if (v > 255) return SOCK_ERR_INVAL;
            opts->ttl = (uint8_t)v;
            if (v) opts->flags |= SOCK_OPT_TTL;
            else opts->flags &= ~SOCK_OPT_TTL;
            return SOCK_OK;
        default:
            return SOCK_ERR_INVAL;
    }
}

int32_t socket_common_options_get(const SocketOptions* opts, int32_t opt, void* value, uint32_t* len) {
    if (!opts || !len) return SOCK_ERR_INVAL;

    uint32_t v = 0;
    switch ((uint32_t)opt) {
        case SOCK_GET_OPT_RECV_TIMEOUT:
            v = opts->recv_timeout_ms;
            break;
        case SOCK_GET_OPT_SEND_TIMEOUT:
            v = opts->send_timeout_ms;
            break;
        case SOCK_GET_OPT_BUF_SIZE:
            v = opts->buf_size;
            break;
        case SOCK_GET_OPT_DEBUG:
            v = opts->debug_level;
            break;
        case SOCK_GET_OPT_DONTFRAG:
            v = (opts->flags & SOCK_OPT_DONTFRAG) != 0;
            break;
        case SOCK_GET_OPT_BROADCAST_ALLOWED:
            v = (opts->flags & SOCK_OPT_BROADCAST_ALLOWED) != 0;
            break;
        case SOCK_GET_OPT_TTL:
            v = opts->ttl;
            break;
        default:
            return SOCK_ERR_INVAL;
    }

    if (!value) {
        *len = sizeof(uint32_t);
        return SOCK_OK;
    }
    if (*len < sizeof(uint32_t)) return SOCK_ERR_INVAL;
    memcpy(value, &v, sizeof(v));
    *len = sizeof(uint32_t);
    return SOCK_OK;
}

socket_impl_t socket_core_impl(ksocket_t* socket) {
    return socket ? socket->impl : NULL;
}

protocol_t socket_core_protocol(const ksocket_t* socket) {
    return socket ? socket->protocol : PROTO_NONE;
}


uint16_t socket_core_pid(const ksocket_t* socket) {
    return socket ? socket->pid : 0;
}


bool socket_core_is_closing(const ksocket_t* socket) {
    return !socket || socket->closing;
}

socket_handle_t socket_core_export_handle(const ksocket_t* socket) {
    if (!socket || !socket->id || !socket->generation) return 0;
    return (socket->generation << SOCKET_HANDLE_INDEX_BITS) | socket->id;
}
