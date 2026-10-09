#include "csocket_packet.h"
#include "networking/network.h"
#include "networking/link_layer/eth.h"
#include "networking/interface_manager.h"
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
    uint32_t generation;
    uint8_t ifindex;
    bool strip_vlan;
} packet_rx_entry_t;

typedef struct packet_socket {
    ksocket_t* ownerSocket;
    SocketOptions options;
    bool registered;
    SockBindSpec bind_spec;
    uint32_t bind_generation;
    SockBindSpec last_rx_spec;
    packet_rx_entry_t* ring;
    uint32_t ring_cap;
    uint32_t head;
    uint32_t tail;
    uint32_t rx_bytes;
} packet_socket_t;

typedef struct packet_frame {
    l2_interface_t* physical;
    l2_interface_t* logical;
    uint32_t raw_len;
    uint32_t logical_len;
    uint16_t ethertype;
    uint16_t logical_type;
    uint16_t vid;
    bool tagged;
    bool logical_valid;
} packet_frame_t;

typedef struct packet_delivery {
    packet_socket_t* socket;
    uint32_t generation;
    uint8_t ifindex;
    bool strip_vlan;
} packet_delivery_t;

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
        memset(&s->ring[i], 0, sizeof(s->ring[i]));
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

    uint32_t supported = SOCK_OPT_RECV_TIMEOUT | SOCK_OPT_BUF_SIZE | SOCK_OPT_DEBUG | SOCK_OPT_FILTER | SOCK_OPT_SPECIAL | SOCK_OPT_NONBLOCK | SOCK_OPT_PACKET_TRUNK;
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
        if (extra->flags & SOCK_OPT_PACKET_TRUNK) s->options.flags |= SOCK_OPT_PACKET_TRUNK;
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
        case SOCK_OPT_PACKET_TRUNK:
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
            v = s->bind_spec.kind != BIND_ANY && (s->bind_spec.kind != BIND_L2 || s->bind_generation != 0);
            break;
        case SOCK_GET_RECV_QUEUED:
            v = s->rx_bytes;
            break;
        case SOCK_GET_OPT_RECV_TIMEOUT:
        case SOCK_GET_OPT_DEBUG:
        case SOCK_GET_OPT_BUF_SIZE:
        case SOCK_GET_OPT_NONBLOCK:
        case SOCK_GET_OPT_PACKET_TRUNK:
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
        case SOCK_GET_TCP_URGENT_REMAINING:
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
        s->bind_generation = 0;
        memset(&s->last_rx_spec, 0, sizeof(s->last_rx_spec));
        s->last_rx_spec.kind = BIND_ANY;
        return SOCK_OK;
    }

    if (spec->kind != BIND_L2 || !spec->ifindex) return SOCK_ERR_INVAL;

    next.kind = BIND_L2;
    next.ifindex = spec->ifindex;
    l2_interface_t* l2 = l2_interface_find_by_index(next.ifindex);
    if (!l2) return SOCK_ERR_INVAL;
    s->bind_spec = next;
    s->bind_generation = l2->generation;
    memset(&s->last_rx_spec, 0, sizeof(s->last_rx_spec));
    s->last_rx_spec.kind = BIND_ANY;
    return SOCK_OK;
}

int64_t socket_recv_packet(socket_impl_t sh, void* buf, uint64_t len) {
    packet_socket_t* s = (packet_socket_t*)sh;
    if (!s || (!buf && len) || len > UINT32_MAX) return SOCK_ERR_INVAL;

    uint32_t start_ms = (uint32_t)get_time();
    packet_rx_entry_t entry;
    uint32_t pkt_len = 0;
    for (;;) {
        irq_flags_t irq = irq_save_disable();
        if (s->head != s->tail) {
            uint32_t pos = s->head;
            entry = s->ring[pos];
            memset(&s->ring[pos], 0, sizeof(s->ring[pos]));
            s->head = (s->head + 1) % s->ring_cap;
            pkt_len = netpkt_len(entry.pkt);
            if (entry.strip_vlan && pkt_len >= ETH_VLAN_TAG_LEN) pkt_len -= ETH_VLAN_TAG_LEN;
            if (s->rx_bytes >= pkt_len) s->rx_bytes -= pkt_len;
            else s->rx_bytes = 0;
            irq_restore(irq);
            break;
        }
        irq_restore(irq);

        if (s->options.flags & SOCK_OPT_NONBLOCK) return SOCK_ERR_WOULDBLOCK;
        if ((s->options.flags & SOCK_OPT_RECV_TIMEOUT) && s->options.recv_timeout_ms) {
            uint32_t now_ms = (uint32_t)get_time();
            uint32_t elapsed_ms = now_ms - start_ms;
            if (elapsed_ms >= s->options.recv_timeout_ms) return SOCK_ERR_WOULDBLOCK;
            uint32_t wait_ms = s->options.recv_timeout_ms - elapsed_ms;
            if (wait_ms > 5) wait_ms = 5;
            msleep(wait_ms);
        }else msleep(5);
    }

    l2_interface_t* source = l2_interface_find_by_index(entry.ifindex);
    if (!source || source->generation != entry.generation) {
        netpkt_unref(entry.pkt);
        return SOCK_ERR_NOT_FOUND;
    }

    irq_flags_t irq = irq_save_disable();
    memset(&s->last_rx_spec, 0, sizeof(s->last_rx_spec));
    s->last_rx_spec.kind = BIND_L2;
    s->last_rx_spec.ifindex = entry.ifindex;
    irq_restore(irq);

    uint32_t n = pkt_len;
    if (n > len) n = (uint32_t)len;
    bool copied = true;
    if (n && entry.strip_vlan) {
        uint32_t addr_len = MAC_ADDR_LEN * 2;
        uint32_t first = n < addr_len ? n : addr_len;
        copied = netpkt_copyout(entry.pkt, 0, buf, first);
        if (copied && n > addr_len) copied = netpkt_copyout(entry.pkt, addr_len + ETH_VLAN_TAG_LEN, (uint8_t*)buf + addr_len, n - addr_len);
    } else if (n) copied = netpkt_copyout(entry.pkt, 0, buf, n);
    netpkt_unref(entry.pkt);
    if (copied) return n;
    return 0;
}

void socket_packet_l2_deleted(socket_impl_t sh, uint8_t ifindex, uint32_t generation) {
    packet_socket_t* s = (packet_socket_t*)sh;
    if (!s || !ifindex) return;

    for (;;) {
        netpkt_t* stale = NULL;
        irq_flags_t irq = irq_save_disable();
        if (s->bind_spec.kind == BIND_L2 && s->bind_spec.ifindex == ifindex && s->bind_generation == generation) s->bind_generation = 0;
        if (s->last_rx_spec.kind == BIND_L2 && s->last_rx_spec.ifindex == ifindex) {
            memset(&s->last_rx_spec, 0, sizeof(s->last_rx_spec));
            s->last_rx_spec.kind = BIND_ANY;
        }

        if (s->ring && s->ring_cap) { 
            for (uint32_t pos = s->head; pos != s->tail; pos = (pos + 1) % s->ring_cap) {
                packet_rx_entry_t* entry = &s->ring[pos];
                if (entry->ifindex != ifindex || entry->generation != generation) continue;

                stale = entry->pkt;
                uint32_t stale_len = netpkt_len(entry->pkt);
                if (entry->strip_vlan && stale_len >= ETH_VLAN_TAG_LEN) stale_len -= ETH_VLAN_TAG_LEN;
                uint32_t cur = pos;
                uint32_t next = (cur + 1) % s->ring_cap;
                while (next != s->tail) {
                    s->ring[cur] = s->ring[next];
                    cur = next;
                    next = (next + 1) % s->ring_cap;
                }
                memset(&s->ring[cur], 0, sizeof(s->ring[cur]));
                s->tail = cur;
                if (s->rx_bytes >= stale_len) s->rx_bytes -= stale_len;
                else s->rx_bytes = 0;
                break;
            }
        }
        irq_restore(irq);
        if (!stale) return;
        netpkt_unref(stale);
    }
}

bool socket_packet_input(uint8_t ifindex, netpkt_t* pkt) {
    if (!ifindex || !pkt) return false;
    packet_frame_t frame = {0};
    frame.raw_len = netpkt_len(pkt);
    if (frame.raw_len < sizeof(eth_hdr_t)) return false;

    frame.physical = l2_interface_find_by_index(ifindex);
    if (!frame.physical) return false;

    frame.ethertype = eth_parse_type(pkt);
    frame.logical_type = frame.ethertype;
    frame.tagged = frame.ethertype == ETHERTYPE_VLAN1Q && eth_parse_vlan(pkt, &frame.vid, &frame.logical_type);
    frame.logical = frame.physical;
    frame.logical_valid = frame.ethertype != ETHERTYPE_VLAN1Q || frame.tagged;
    if (frame.tagged) {
        if (frame.vid == 4095) frame.logical_valid = false;
        else if (frame.vid) {
            frame.logical = l2_vlan_find(ifindex, frame.vid);
            frame.logical_valid = frame.logical && frame.logical->is_up;
        }
    }
    frame.logical_len = frame.tagged ? frame.raw_len - ETH_VLAN_TAG_LEN : frame.raw_len;

    packet_delivery_t targets[PACKET_SOCKET_MAX];
    int n = 0;
    irq_flags_t irq = irq_save_disable();
    for (int i = 0; i < PACKET_SOCKET_MAX; i++) {
        packet_socket_t* s = g_packet_sockets[i];
        if (!s || socket_core_is_closing(s->ownerSocket)) continue;

        bool trunk = (s->options.flags & SOCK_OPT_PACKET_TRUNK) != 0;
        l2_interface_t *source = NULL;
        uint32_t pkt_len = frame.raw_len;
        uint16_t ethertype = frame.ethertype;
        bool strip_vlan = false;

        if (trunk) {
            if (s->bind_spec.kind == BIND_L2 && s->bind_spec.ifindex != ifindex) {
                if (!frame.tagged || !frame.vid || !frame.logical_valid || s->bind_spec.ifindex != frame.logical->ifindex) continue;
                source = frame.logical;
            } else source = frame.physical;
        } else {
            if (!frame.logical_valid) continue;
            source = frame.logical;
            if (s->bind_spec.kind == BIND_L2 && s->bind_spec.ifindex != source->ifindex) continue;
            if (frame.tagged) {
                pkt_len = frame.logical_len;
                ethertype = frame.logical_type;
                strip_vlan = true;
            }
        }

        if (!source) continue;
        if (s->bind_spec.kind == BIND_L2 && s->bind_generation != source->generation) continue;
        if ((s->options.packet_filter.flags & SOCKET_PACKET_FILTER_HAS_ETHERTYPE) && s->options.packet_filter.ethertype != ethertype) continue;
        if ((s->options.packet_filter.flags & SOCKET_PACKET_FILTER_HAS_MIN_LEN) && pkt_len < s->options.packet_filter.min_len) continue;
        if ((s->options.packet_filter.flags & SOCKET_PACKET_FILTER_HAS_MAX_LEN) && pkt_len > s->options.packet_filter.max_len) continue;
        socket_core_ref(s->ownerSocket);
        targets[n].socket = s;
        targets[n].ifindex = source->ifindex;
        targets[n].generation = source->generation;
        targets[n].strip_vlan = strip_vlan;
        n++;
    }
    irq_restore(irq);

    if (!n) return false;

    netpkt_t* view = netpkt_view(pkt, 0, frame.raw_len);
    if (!view) {
        for (int i = 0; i < n; i++) socket_core_put(targets[i].socket->ownerSocket);
        return false;
    }

    bool delivered = false;
    for (int i = 0; i < n; i++) {
        packet_delivery_t* target = &targets[i];
        packet_socket_t* s = target->socket;
        uint32_t limit = s->options.buf_size ? s->options.buf_size : PACKET_RX_DEFAULT_BUF_SIZE;
        uint32_t pkt_len = target->strip_vlan ? frame.logical_len : frame.raw_len;
        if (pkt_len > limit) {
            socket_core_put(s->ownerSocket);
            continue;
        }

        irq = irq_save_disable();
        l2_interface_t* source = l2_interface_find_by_index(target->ifindex);
        bool same_bind = s->bind_spec.kind != BIND_L2 || s->bind_generation == target->generation;
        bool can_queue = source && source->generation == target->generation && same_bind && s->ring && s->ring_cap && s->rx_bytes <= limit - pkt_len;
        uint32_t nexti = 0;
        if (can_queue) {
            nexti = (s->tail + 1) % s->ring_cap;
            if (nexti == s->head) can_queue = false;
        }
        if (can_queue) {
            packet_rx_entry_t* entry = &s->ring[s->tail];
            netpkt_ref(view);
            entry->pkt = view;
            entry->ifindex = target->ifindex;
            entry->generation = target->generation;
            entry->strip_vlan = target->strip_vlan;
            s->tail = nexti;
            s->rx_bytes += pkt_len;
            delivered = true;
        }
        irq_restore(irq);
        socket_core_put(s->ownerSocket);
    }

    netpkt_unref(view);
    return delivered;
}
