#include "csocket_special.h"
#include "alloc/allocate.h"
#include "std/memory.h"

typedef struct special_socket {
    ksocket_t* ownerSocket;
    SocketOptions options;
} special_socket_t;

socket_impl_t socket_special_create(ksocket_t* owner, const SocketOptions* extra) {
    if (!owner) return NULL;

    special_socket_t* s = (special_socket_t*)zalloc(sizeof(special_socket_t));
    if (!s) return NULL;

    s->ownerSocket = owner;
    if (extra) s->options = *extra;
    return s;
}

void socket_destroy_special(socket_impl_t sh) {
    special_socket_t* s = (special_socket_t*)sh;
    if (!s) return;
    release(s);
}

int32_t socket_close_special(socket_impl_t sh) {
    return sh ? SOCK_OK : SOCK_ERR_INVAL;
}

int32_t socket_setopt_special(socket_impl_t sh, int32_t opt, const void* value, uint32_t len) {
    special_socket_t* s = (special_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;
    return socket_common_options_set(&s->options, opt, value, len);
}

int32_t socket_getopt_special(socket_impl_t sh, int32_t opt, void* value, uint32_t* len) {
    special_socket_t* s = (special_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;
    return socket_common_options_get(&s->options, opt, value, len);
}
