#include "csocket_special.h"
#include "net_ctrl.h"
#include "alloc/allocate.h"
#include "std/memory.h"

typedef struct special_socket {
    ksocket_t* ownerSocket;
    uint8_t* rx_buf;
    uint32_t rx_len;
    uint32_t rx_off;
} special_socket_t;

socket_impl_t socket_special_create(ksocket_t* owner, const SocketOptions* extra) {
    if (!owner) return NULL;

    special_socket_t* s = (special_socket_t*)zalloc(sizeof(special_socket_t));
    if (!s) return NULL;

    s->ownerSocket = owner;
    return s;
}

void socket_destroy_special(socket_impl_t sh) {
    special_socket_t* s = (special_socket_t*)sh;
    if (!s) return;
    if (s->rx_buf) release(s->rx_buf);
    release(s);
}

int32_t socket_close_special(socket_impl_t sh) {
    return sh ? SOCK_OK : SOCK_ERR_INVAL;
}

int32_t socket_setopt_special(socket_impl_t sh, int32_t opt, const void* value, uint32_t len) {
    return sh ? SOCK_ERR_UNSUP : SOCK_ERR_INVAL;
}

int32_t socket_getopt_special(socket_impl_t sh, int32_t opt, void* value, uint32_t* len) {
    return sh ? SOCK_ERR_UNSUP : SOCK_ERR_INVAL;
}

int64_t socket_send_special(socket_impl_t sh, const void* buf, uint64_t len) {
    special_socket_t* s = (special_socket_t*)sh;
    if (!s || (!buf && len) || len > UINT32_MAX) return SOCK_ERR_INVAL;
    if (socket_core_special_kind(s->ownerSocket) != SOCKET_SPECIAL_CTRL) return SOCK_ERR_UNSUP;

    if (s->rx_buf) {
        release(s->rx_buf);
        s->rx_buf = NULL;
        s->rx_len = 0;
        s->rx_off = 0;
    }

    uint8_t* out = NULL;
    uint32_t out_len = 0;
    int32_t rc = net_ctrl_dispatch(buf, (uint32_t)len, &out, &out_len);
    if (rc != SOCK_OK) return rc;

    s->rx_buf = out;
    s->rx_len = out_len;
    s->rx_off = 0;
    return (int64_t)len;
}

int64_t socket_recv_special(socket_impl_t sh, void* buf, uint64_t len) {
    special_socket_t* s = (special_socket_t*)sh;
    if (!s || (!buf && len) || len > UINT32_MAX) return SOCK_ERR_INVAL;
    if (socket_core_special_kind(s->ownerSocket) != SOCKET_SPECIAL_CTRL) return SOCK_ERR_UNSUP;
    if (!s->rx_buf || s->rx_off >= s->rx_len) return SOCK_ERR_WOULDBLOCK;

    uint32_t avail = s->rx_len - s->rx_off;
    uint32_t n = avail < (uint32_t)len ? avail : (uint32_t)len;
    if (n) memcpy(buf, s->rx_buf + s->rx_off, n);
    s->rx_off += n;
    if (s->rx_off >= s->rx_len) {
        release(s->rx_buf);
        s->rx_buf = NULL;
        s->rx_len = 0;
        s->rx_off = 0;
    }
    return n;
}
