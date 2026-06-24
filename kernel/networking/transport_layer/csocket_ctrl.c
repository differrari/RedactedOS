#include "csocket_ctrl.h"
#include "net_ctrl.h"
#include "alloc/allocate.h"
#include "std/memory.h"

typedef struct ctrl_socket {
    uint8_t* rx_buf;
    uint32_t rx_len;
    uint32_t rx_off;
} ctrl_socket_t;

socket_impl_t socket_ctrl_create(ksocket_t* owner, const SocketOptions* extra) {
    if (!owner) return NULL;
    if (socket_core_special_kind(owner) != SOCKET_SPECIAL_CTRL) return NULL;
    if (extra && (extra->flags & ~SOCK_OPT_SPECIAL)) return NULL;

    ctrl_socket_t* s = (ctrl_socket_t*)zalloc(sizeof(ctrl_socket_t));
    if (!s) return NULL;

    return s;
}

void socket_destroy_ctrl(socket_impl_t sh) {
    ctrl_socket_t* s = (ctrl_socket_t*)sh;
    if (!s) return;
    if (s->rx_buf) release(s->rx_buf);
    release(s);
}

int32_t socket_close_ctrl(socket_impl_t sh) {
    return sh ? SOCK_OK : SOCK_ERR_INVAL;
}

int32_t socket_setopt_ctrl(socket_impl_t sh, int32_t opt, const void* value, uint32_t len) {
    return sh ? SOCK_ERR_UNSUP : SOCK_ERR_INVAL;
}

int32_t socket_getopt_ctrl(socket_impl_t sh, int32_t opt, void* value, uint32_t* len) {
    ctrl_socket_t* s = (ctrl_socket_t*)sh;
    if (!s || !len) return SOCK_ERR_INVAL;

    if ((uint32_t)opt == SOCK_GET_BIND_SPEC) {
        SockBindSpec spec;
        memset(&spec, 0, sizeof(spec));
        spec.kind = BIND_ANY;
        return socket_common_get_value(&spec, sizeof(spec), value, len);
    }

    if ((uint32_t)opt == SOCK_GET_LAST_RX_SPEC) return SOCK_ERR_UNSUP;

    uint32_t v = 0;
    switch ((uint32_t)opt) {
        case SOCK_GET_BOUND:
            break;
        case SOCK_GET_RECV_QUEUED:
            if (s->rx_buf && s->rx_off < s->rx_len) v = s->rx_len - s->rx_off;
            break;
        case SOCK_GET_CONNECTED:
        case SOCK_GET_LISTENING:
        case SOCK_GET_LOCAL_PORT:
        case SOCK_GET_SEND_QUEUED:
        case SOCK_GET_OPT_RECV_TIMEOUT:
        case SOCK_GET_OPT_SEND_TIMEOUT:
        case SOCK_GET_OPT_BUF_SIZE:
        case SOCK_GET_OPT_DEBUG:
        case SOCK_GET_OPT_DONTFRAG:
        case SOCK_GET_OPT_TTL:
        case SOCK_GET_OPT_KEEPALIVE:
        case SOCK_GET_OPT_KEEPALIVE_INTERVAL:
        case SOCK_GET_OPT_TCP_NO_DELAY:
        case SOCK_GET_OPT_SEND_BUF_SIZE:
        case SOCK_GET_OPT_BROADCAST_ALLOWED:
        case SOCK_GET_OPT_FILTER:
        case SOCK_GET_MCAST_GROUPS:
        case SOCK_GET_TCP_STATE:
        case SOCK_GET_TCP_MSS:
        case SOCK_GET_TCP_RTT_MS:
        case SOCK_GET_TCP_RETRANSMITS:
            return SOCK_ERR_UNSUP;
        default:
            return SOCK_ERR_INVAL;
    }

    return socket_common_get_value(&v, sizeof(v), value, len);
}

int64_t socket_send_ctrl(socket_impl_t sh, const void* buf, uint64_t len) {
    ctrl_socket_t* s = (ctrl_socket_t*)sh;
    if (!s || (!buf && len) || len > UINT32_MAX) return SOCK_ERR_INVAL;

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

int64_t socket_recv_ctrl(socket_impl_t sh, void* buf, uint64_t len) {
    ctrl_socket_t* s = (ctrl_socket_t*)sh;
    if (!s || (!buf && len) || len > UINT32_MAX) return SOCK_ERR_INVAL;
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
