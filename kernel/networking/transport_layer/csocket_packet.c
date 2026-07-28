#include "csocket_packet.h"
#include "networking/network.h"
#include "networking/link_layer/eth.h"
#include "alloc/allocate.h"
#include "exceptions/irq.h"
#include "std/memory.h"
#include "syscalls/syscalls.h"

#define PACKET_SOCKET_MAX 64
#define PACKET_RX_DEFAULT_RING_CAP 64
#define PACKET_RX_MAX_RING_CAP 1024
#define PACKET_RX_DEFAULT_BUF_SIZE (MAX_PACKET_SIZE * PACKET_RX_DEFAULT_RING_CAP)

typedef struct packet_rx_entry {
    netpkt_t* pkt;
    uint8_t ifindex;
} packet_rx_entry_t;

typedef struct packet_socket {
    ksocket_t* ownerSocket;
    SocketOptions options;
    bool registered;
    SockBindSpec bind_spec;
    SockBindSpec last_rx_spec;
    packet_rx_entry_t* ring;
    uint32_t ring_cap;
    uint32_t head;
    uint32_t tail;
    uint32_t rx_bytes;
} packet_socket_t;

static packet_socket_t* g_packet_sockets[PACKET_SOCKET_MAX];

static void packet_socket_unregister(packet_socket_t* s) {
    if (!s || !s->registered) return;

    irq_flags_t irq = irq_save_disable();
    for (int i = 0; i < PACKET_SOCKET_MAX; i++) {
        if (g_packet_sockets[i] == s) {
            g_packet_sockets[i] = NULL;
            break;
        }
    }
    s->registered = false;
    irq_restore(irq);
}

static void packet_socket_clear_rx(packet_socket_t* s) {
    if (!s || !s->ring) return;

    for (uint32_t i = 0; i < s->ring_cap; i++) {
        if (s->ring[i].pkt) netpkt_unref(s->ring[i].pkt);
        s->ring[i].pkt = NULL;
        s->ring[i].ifindex = 0;
    }

    release(s->ring);
    s->ring = NULL;
    s->ring_cap = 0;
    s->head = 0;
    s->tail = 0;
    s->rx_bytes = 0;
}

static int32_t packet_set_filter(packet_socket_t* s, const void* value, uint32_t len) {
    if (!s) return SOCK_ERR_INVAL;
    if (!value && !len) {
        memset(&s->options.packet_filter, 0, sizeof(s->options.packet_filter));
        s->options.flags &= ~SOCK_OPT_FILTER;
        return SOCK_OK;
    }
    if (!value || len != sizeof(SocketPacketFilter)) return SOCK_ERR_INVAL;

    SocketPacketFilter filter;
    memcpy(&filter, value, sizeof(filter));

    uint32_t valid_flags = SOCKET_PACKET_FILTER_HAS_ETHERTYPE | SOCKET_PACKET_FILTER_HAS_MIN_LEN | SOCKET_PACKET_FILTER_HAS_MAX_LEN;
    if (filter.reserved || (filter.flags & ~valid_flags)) return SOCK_ERR_INVAL;
    if ((filter.flags & SOCKET_PACKET_FILTER_HAS_ETHERTYPE) && !filter.ethertype) return SOCK_ERR_INVAL;
    if (!(filter.flags & SOCKET_PACKET_FILTER_HAS_ETHERTYPE) && filter.ethertype) return SOCK_ERR_INVAL;
    if ((filter.flags & SOCKET_PACKET_FILTER_HAS_MIN_LEN) && !filter.min_len) return SOCK_ERR_INVAL;
    if (!(filter.flags & SOCKET_PACKET_FILTER_HAS_MIN_LEN) && filter.min_len) return SOCK_ERR_INVAL;
    if ((filter.flags & SOCKET_PACKET_FILTER_HAS_MAX_LEN) && !filter.max_len) return SOCK_ERR_INVAL;
    if (!(filter.flags & SOCKET_PACKET_FILTER_HAS_MAX_LEN) && filter.max_len) return SOCK_ERR_INVAL;
    if ((filter.flags & SOCKET_PACKET_FILTER_HAS_MIN_LEN) && (filter.flags & SOCKET_PACKET_FILTER_HAS_MAX_LEN) && filter.min_len > filter.max_len) return SOCK_ERR_INVAL;

    s->options.packet_filter = filter;
    if (filter.flags) s->options.flags |= SOCK_OPT_FILTER;
    else s->options.flags &= ~SOCK_OPT_FILTER;
    return SOCK_OK;
}

socket_impl_t socket_packet_create(ksocket_t* owner, const SocketOptions* extra) {
    if (!owner) return NULL;
    if (socket_core_special_kind(owner) != SOCKET_SPECIAL_PACKET) return NULL;

    uint32_t supported = SOCK_OPT_RECV_TIMEOUT | SOCK_OPT_BUF_SIZE | SOCK_OPT_DEBUG | SOCK_OPT_FILTER | SOCK_OPT_SPECIAL | SOCK_OPT_NONBLOCK;
    if (extra && (extra->flags & ~supported)) return NULL;

    packet_socket_t* s = (packet_socket_t*)zalloc(sizeof(packet_socket_t));
    if (!s) return NULL;

    s->ownerSocket = owner;
    s->options.flags = SOCK_OPT_SPECIAL;
    s->options.special_kind = SOCKET_SPECIAL_PACKET;
    s->options.buf_size = PACKET_RX_DEFAULT_BUF_SIZE;
    s->bind_spec.kind = BIND_ANY;
    s->last_rx_spec.kind = BIND_ANY;

    if (extra) {
        if ((extra->flags & SOCK_OPT_DEBUG) && extra->debug_level > SOCK_DBG_ALL) {
            release(s);
            return 0;
        }
        if (extra->flags & SOCK_OPT_DEBUG) {
            s->options.flags |= SOCK_OPT_DEBUG;
            s->options.debug_level = extra->debug_level;
        }
        if (extra->flags & SOCK_OPT_RECV_TIMEOUT) {
            s->options.flags |= SOCK_OPT_RECV_TIMEOUT;
            s->options.recv_timeout_ms = extra->recv_timeout_ms;
        }
        if (extra->flags & SOCK_OPT_NONBLOCK) s->options.flags |= SOCK_OPT_NONBLOCK;
        if (extra->flags & SOCK_OPT_BUF_SIZE) {
            if (!extra->buf_size) {
                release(s);
                return 0;
            }
            s->options.flags |= SOCK_OPT_BUF_SIZE;
            s->options.buf_size = extra->buf_size;
        }
        if (extra->flags & SOCK_OPT_FILTER) {
            if (packet_set_filter(s, &extra->packet_filter, sizeof(extra->packet_filter)) != SOCK_OK) {
                release(s);
                return NULL;
            }
        }
    }

    uint32_t usable = s->options.buf_size/MAX_PACKET_SIZE;
    if (usable < 4) usable = 4;
    if (usable > PACKET_RX_MAX_RING_CAP) usable = PACKET_RX_MAX_RING_CAP;

    s->ring_cap = usable+1;
    s->ring = (packet_rx_entry_t*)zalloc(sizeof(packet_rx_entry_t) * s->ring_cap);
    if (!s->ring) {
        release(s);
        return NULL;
    }

    irq_flags_t irq = irq_save_disable();
    for (int i = 0; i < PACKET_SOCKET_MAX; i++) {
        if (!g_packet_sockets[i]) {
            g_packet_sockets[i] = s;
            s->registered = true;
            irq_restore(irq);
            return s;
        }
    }
    irq_restore(irq);

    packet_socket_clear_rx(s);
    release(s);
    return NULL;
}

void socket_destroy_packet(socket_impl_t sh) {
    packet_socket_t* s = (packet_socket_t*)sh;
    if (!s) return;

    packet_socket_unregister(s);
    packet_socket_clear_rx(s);
    release(s);
}

int32_t socket_close_packet(socket_impl_t sh) {
    packet_socket_t* s = (packet_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;

    packet_socket_unregister(s);
    return SOCK_OK;
}

int32_t socket_setopt_packet(socket_impl_t sh, int32_t opt, const void* value, uint32_t len) {
    packet_socket_t* s = (packet_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;

    switch ((uint32_t)opt) {
        case SOCK_OPT_RECV_TIMEOUT:
        case SOCK_OPT_DEBUG:
        case SOCK_OPT_NONBLOCK:
            return socket_common_options_set(&s->options, opt, value, len);
        case SOCK_OPT_FILTER:
            return packet_set_filter(s, value, len);
        case SOCK_OPT_BUF_SIZE:
        case SOCK_OPT_SEND_TIMEOUT:
        case SOCK_OPT_SEND_BUF_SIZE:
        case SOCK_OPT_KEEPALIVE:
        case SOCK_OPT_KEEPALIVE_INTERVAL:
        case SOCK_OPT_TCP_NO_DELAY:
        case SOCK_OPT_BROADCAST_ALLOWED:
        case SOCK_OPT_SPECIAL:
        case SOCK_OPT_MCAST_JOIN:
        case SOCK_OPT_MCAST_LEAVE:
        case SOCK_OPT_DONTFRAG:
        case SOCK_OPT_TTL:
            return SOCK_ERR_UNSUP;
        default:
            return SOCK_ERR_INVAL;
    }
}

int32_t socket_getopt_packet(socket_impl_t sh, int32_t opt, void* value, uint32_t* len) {
    packet_socket_t* s = (packet_socket_t*)sh;
    if (!s || !len) return SOCK_ERR_INVAL;

    switch ((uint32_t)opt) {
        case SOCK_GET_BIND_SPEC:
            return socket_common_get_value(&s->bind_spec, sizeof(s->bind_spec), value, len);
        case SOCK_GET_LAST_RX_SPEC:
            return socket_common_get_value(&s->last_rx_spec, sizeof(s->last_rx_spec), value, len);
        case SOCK_GET_OPT_FILTER:
            return socket_common_get_value(&s->options.packet_filter, sizeof(s->options.packet_filter), value, len);
        default:
            break;
    }

    uint32_t v = 0;
    switch ((uint32_t)opt) {
        case SOCK_GET_BOUND:
            v = s->bind_spec.kind != BIND_ANY;
            break;
        case SOCK_GET_RECV_QUEUED:
            v = s->rx_bytes;
            break;
        case SOCK_GET_OPT_RECV_TIMEOUT:
        case SOCK_GET_OPT_DEBUG:
        case SOCK_GET_OPT_BUF_SIZE:
        case SOCK_GET_OPT_NONBLOCK:
            return socket_common_options_get(&s->options, opt, value, len);
        case SOCK_GET_CONNECTED:
        case SOCK_GET_LISTENING:
        case SOCK_GET_LOCAL_PORT:
        case SOCK_GET_SEND_QUEUED:
        case SOCK_GET_OPT_SEND_TIMEOUT:
        case SOCK_GET_OPT_SEND_BUF_SIZE:
        case SOCK_GET_OPT_KEEPALIVE:
        case SOCK_GET_OPT_KEEPALIVE_INTERVAL:
        case SOCK_GET_OPT_TCP_NO_DELAY:
        case SOCK_GET_OPT_BROADCAST_ALLOWED:
        case SOCK_GET_OPT_DONTFRAG:
        case SOCK_GET_OPT_TTL:
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

int32_t socket_bind_packet(socket_impl_t sh, const SockBindSpec* spec) {
    packet_socket_t* s = (packet_socket_t*)sh;
    if (!s || !spec) return SOCK_ERR_INVAL;

    SockBindSpec next;
    memset(&next, 0, sizeof(next));

    if (spec->kind == BIND_ANY) {
        next.kind = BIND_ANY;
        s->bind_spec = next;
        memset(&s->last_rx_spec, 0, sizeof(s->last_rx_spec));
        s->last_rx_spec.kind = BIND_ANY;
        return SOCK_OK;
    }

    if (spec->kind != BIND_L2 || !spec->ifindex || !network_get_ifname(spec->ifindex)) return SOCK_ERR_INVAL;

    next.kind = BIND_L2;
    next.ifindex = spec->ifindex;
    s->bind_spec = next;
    memset(&s->last_rx_spec, 0, sizeof(s->last_rx_spec));
    s->last_rx_spec.kind = BIND_ANY;
    return SOCK_OK;
}

int64_t socket_recv_packet(socket_impl_t sh, void* buf, uint64_t len) {
    packet_socket_t* s = (packet_socket_t*)sh;
    if (!s || (!buf && len) || len > UINT32_MAX) return SOCK_ERR_INVAL;

    for (;;) {
        irq_flags_t irq = irq_save_disable();
        bool ready = s->head != s->tail;
        irq_restore(irq);
        if (ready) break;
        if (s->options.flags & SOCK_OPT_NONBLOCK) return SOCK_ERR_WOULDBLOCK;

        uint32_t start_ms = (uint32_t)get_time();
        while (1) {
            irq = irq_save_disable();
            ready = s->head != s->tail;
            irq_restore(irq);
            if (ready) break;

            if ((s->options.flags & SOCK_OPT_RECV_TIMEOUT) && s->options.recv_timeout_ms) {
                uint32_t now_ms = (uint32_t)get_time();
                uint32_t elapsed_ms = now_ms - start_ms;
                if (elapsed_ms >= s->options.recv_timeout_ms) return SOCK_ERR_WOULDBLOCK;
                uint32_t wait_ms = s->options.recv_timeout_ms - elapsed_ms;
                if (wait_ms > 5) wait_ms = 5;
                msleep(wait_ms);
            }else msleep(5);
        }
    }

    irq_flags_t irq = irq_save_disable();
    uint32_t pos = s->head;
    netpkt_t* pkt = s->ring[pos].pkt;
    uint8_t ifindex = s->ring[pos].ifindex;
    uint32_t pkt_len = netpkt_len(pkt);
    memset(&s->last_rx_spec, 0, sizeof(s->last_rx_spec));
    s->last_rx_spec.kind = BIND_L2;
    s->last_rx_spec.ifindex = ifindex;
    s->ring[pos].pkt = NULL;
    s->ring[pos].ifindex = 0;
    s->head = (s->head + 1) % s->ring_cap;
    if (s->rx_bytes >= pkt_len) s->rx_bytes -= pkt_len;
    else s->rx_bytes = 0;
    irq_restore(irq);

    uint32_t n = pkt_len;
    if (n > len) n = (uint32_t)len;
    if (n && !netpkt_copyout(pkt, 0, buf, n)) n = 0;
    netpkt_unref(pkt);
    return n;
}

bool socket_packet_input(uint8_t ifindex, netpkt_t* pkt) {
    if (!ifindex || !pkt) return false;
    uint32_t pkt_len = netpkt_len(pkt);
    if (!pkt_len) return false;

    uint16_t ethertype = eth_parse_type(pkt);
    packet_socket_t* targets[PACKET_SOCKET_MAX];
    int n = 0;
    irq_flags_t irq = irq_save_disable();
    for (int i = 0; i < PACKET_SOCKET_MAX; i++) {
        packet_socket_t* s = g_packet_sockets[i];
        if (!s || socket_core_is_closing(s->ownerSocket)) continue;
        if (s->bind_spec.kind == BIND_L2 && s->bind_spec.ifindex != ifindex) continue;
        if ((s->options.packet_filter.flags & SOCKET_PACKET_FILTER_HAS_ETHERTYPE) && s->options.packet_filter.ethertype != ethertype) continue;
        if ((s->options.packet_filter.flags & SOCKET_PACKET_FILTER_HAS_MIN_LEN) && pkt_len < s->options.packet_filter.min_len) continue;
        if ((s->options.packet_filter.flags & SOCKET_PACKET_FILTER_HAS_MAX_LEN) && pkt_len > s->options.packet_filter.max_len) continue;
        socket_core_ref(s->ownerSocket);
        targets[n++] = s;
    }
    irq_restore(irq);

    if (!n) return false;

    bool delivered = false;
    for (int i = 0; i < n; i++) {
        packet_socket_t* s = targets[i];
        uint32_t limit = s->options.buf_size ? s->options.buf_size : PACKET_RX_DEFAULT_BUF_SIZE;
        if (pkt_len > limit) {
            socket_core_put(s->ownerSocket);
            continue;
        }

        irq = irq_save_disable();
        bool can_queue = s->ring && s->ring_cap && s->rx_bytes <= limit - pkt_len;
        uint32_t nexti = can_queue ? (s->tail + 1) % s->ring_cap : 0;
        if (can_queue && nexti == s->head) can_queue = false;
        irq_restore(irq);

        if (!can_queue){
            socket_core_put(s->ownerSocket);
            continue;
        }

        netpkt_t* view = netpkt_view(pkt, 0, pkt_len);
        if (!view) {
            socket_core_put(s->ownerSocket);
            continue;
        }

        irq = irq_save_disable();
        nexti = (s->tail + 1) % s->ring_cap;
        if (s->ring && nexti != s->head && s->rx_bytes <= limit - pkt_len) {
            s->ring[s->tail].pkt = view;
            s->ring[s->tail].ifindex = ifindex;
            s->tail = nexti;
            s->rx_bytes += pkt_len;
            view = 0;
            delivered = true;
        }
        irq_restore(irq);
        if (view) netpkt_unref(view);
        socket_core_put(s->ownerSocket);
    }
    return delivered;
}
