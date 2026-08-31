#include "csocket_raw.h"
#include "alloc/allocate.h"
#include "exceptions/irq.h"
#include "net/checksums.h"
#include "std/memory.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv6_route.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/link_layer/eth.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/transport_layer/socket_bind.h"
#include "syscalls/syscalls.h"

#define RAW_SOCKET_MAX 64
#define RAW_RX_RING_CAP 16
#define RAW_RX_MAX_BYTES 32768

typedef struct raw_rx_entry {
    uint8_t* data;
    uint32_t len;
    net_l4_endpoint src;
    SockBindSpec rx_spec;
} raw_rx_entry_t; 

typedef struct raw_socket {
    ksocket_t* ownerSocket;
    SocketOptions options;
    bool registered;
    bool bound;
    bool connected;
    SockBindSpec bind_spec;
    SockBindSpec last_rx_spec;
    net_l4_endpoint remote_ep;
    raw_rx_entry_t rx[RAW_RX_RING_CAP];
    uint8_t rx_head;
    uint8_t rx_tail;
    uint8_t rx_count;
    uint32_t rx_bytes;
} raw_socket_t;

static raw_socket_t* g_raw_sockets[RAW_SOCKET_MAX];

static int32_t raw_set_filter(raw_socket_t* s, const void* value, uint32_t len) {
    if (!s) return SOCK_ERR_INVAL;
    if (!value && !len) { 
        memset(&s->options.raw_filter, 0, sizeof(s->options.raw_filter));
        s->options.flags &= ~SOCK_OPT_FILTER;
        return SOCK_OK;
    }
    if (!value || len != sizeof(SocketRawFilter)) return SOCK_ERR_INVAL;

    SocketRawFilter filter;
    memcpy(&filter, value, sizeof(filter));
    if (filter.count > SOCKET_RAW_FILTER_MAX_RULES) return SOCK_ERR_INVAL;
    for (uint32_t i = 0; i < filter.count; i++) {
        SocketRawFilterRule* rule = &filter.rules[i];
        if (rule->reserved || (rule->flags & ~(SOCKET_RAW_FILTER_HAS_CODE | SOCKET_RAW_FILTER_HAS_ID | SOCKET_RAW_FILTER_HAS_SEQ))) return SOCK_ERR_INVAL;
        if (!(rule->flags & SOCKET_RAW_FILTER_HAS_CODE) && rule->code) return SOCK_ERR_INVAL;
        if (!(rule->flags & SOCKET_RAW_FILTER_HAS_ID) && rule->id) return SOCK_ERR_INVAL;
        if (!(rule->flags & SOCKET_RAW_FILTER_HAS_SEQ) && rule->seq) return SOCK_ERR_INVAL;
    }

    s->options.raw_filter = filter;
    if (filter.count) s->options.flags |= SOCK_OPT_FILTER;
    else s->options.flags &= ~SOCK_OPT_FILTER;
    return SOCK_OK;
}

static bool raw_enqueue(raw_socket_t* s, const net_l4_endpoint* src, const SockBindSpec* rx_spec, netpkt_t* pkt, uint32_t len) {
    if (!s || !src || !pkt || !len || len > NETPKT_MAX_ALLOC || len > RAW_RX_MAX_BYTES) return false;

    if (s->options.raw_filter.count) {
        if (netpkt_len(pkt) < 1) return false;
        uint8_t hdr[8];
        uint32_t hdr_len = netpkt_len(pkt) >= sizeof(hdr) ? (uint32_t)sizeof(hdr) : netpkt_len(pkt);
        if (!netpkt_copyout(pkt, 0, hdr, hdr_len)) return false;
        bool ok = false;
        for (uint32_t i = 0; i < s->options.raw_filter.count; i++) {
            const SocketRawFilterRule* rule = &s->options.raw_filter.rules[i];
            if (rule->type != hdr[0]) continue;
            if ((rule->flags & SOCKET_RAW_FILTER_HAS_CODE) && (hdr_len < 2 || rule->code != hdr[1])) continue;
            if ((rule->flags & SOCKET_RAW_FILTER_HAS_ID) && (hdr_len < 6 || rule->id != rd_be16(hdr+4))) continue;
            if ((rule->flags & SOCKET_RAW_FILTER_HAS_SEQ) && (hdr_len < 8 || rule->seq != rd_be16(hdr+6))) continue;
            ok = true;
            break;
        }
        if (!ok) return false;
    }

    uint8_t* copy = (uint8_t*)zalloc(len);
    if (!copy) return false;
    if (!netpkt_copyout(pkt, 0, copy, len)) {
        release(copy);
        return false;
    }

    irq_flags_t irq = irq_save_disable();
    if (s->rx_count >= RAW_RX_RING_CAP || s->rx_bytes > RAW_RX_MAX_BYTES - len) {
        irq_restore(irq);
        release(copy);
        return false;
    }

    uint8_t pos = s->rx_tail;
    s->rx[pos].data = copy;
    s->rx[pos].len = len;
    s->rx[pos].src = *src;
    if (rx_spec) s->rx[pos].rx_spec = *rx_spec;
    else {
        memset(&s->rx[pos].rx_spec, 0, sizeof(s->rx[pos].rx_spec));
        s->rx[pos].rx_spec.kind = BIND_ANY;
    }
    s->rx_tail = (uint8_t)((s->rx_tail + 1) % RAW_RX_RING_CAP);
    s->rx_count++;
    s->rx_bytes += len;
    irq_restore(irq);
    return true;
}

socket_impl_t socket_raw_create(ksocket_t* owner, const SocketOptions* extra) {
    if (!owner || socket_core_special_kind(owner) != SOCKET_SPECIAL_RAW) return NULL;
    protocol_t proto = socket_core_protocol(owner);
    if (proto != PROTO_ICMP && proto != PROTO_IGMP && proto != PROTO_ICMPV6) return NULL;
    //TODO add ESP/AH sock if needed

    uint32_t supported = SOCK_OPT_RECV_TIMEOUT | SOCK_OPT_DEBUG | SOCK_OPT_DONTFRAG | SOCK_OPT_TTL | SOCK_OPT_FILTER | SOCK_OPT_SPECIAL | SOCK_OPT_NONBLOCK | SOCK_OPT_DONTROUTE;
    if (extra && (extra->flags & ~supported)) return NULL;

    raw_socket_t* s = (raw_socket_t*)zalloc(sizeof(raw_socket_t));
    if (!s) return NULL;

    s->ownerSocket = owner;
    s->options.flags = SOCK_OPT_SPECIAL;
    s->options.special_kind = SOCKET_SPECIAL_RAW;
    s->last_rx_spec.kind = BIND_ANY;
    if (extra) {
        if ((extra->flags & SOCK_OPT_RECV_TIMEOUT) && extra->recv_timeout_ms) {
            s->options.flags |= SOCK_OPT_RECV_TIMEOUT;
            s->options.recv_timeout_ms = extra->recv_timeout_ms;
        }

        if (extra->flags & SOCK_OPT_DEBUG) {
            if (extra->debug_level > SOCK_DBG_ALL) {
                release(s);
                return NULL;
            }
            s->options.flags |= SOCK_OPT_DEBUG;
            s->options.debug_level = extra->debug_level;
        }

        if (extra->flags & SOCK_OPT_DONTFRAG) s->options.flags |= SOCK_OPT_DONTFRAG;
        if (extra->flags & SOCK_OPT_NONBLOCK) s->options.flags |= SOCK_OPT_NONBLOCK;
        if (extra->flags & SOCK_OPT_DONTROUTE) s->options.flags |= SOCK_OPT_DONTROUTE;
        if ((extra->flags & SOCK_OPT_TTL) && extra->ttl) {
            s->options.flags |= SOCK_OPT_TTL;
            s->options.ttl = extra->ttl;
        }
        if (extra->flags & SOCK_OPT_FILTER) {
            if (raw_set_filter(s, &extra->raw_filter, sizeof(extra->raw_filter)) != SOCK_OK) {
                release(s);
                return NULL;
            }
        }
    }

    irq_flags_t irq = irq_save_disable();
    for (int i = 0; i < RAW_SOCKET_MAX; i++) {
        if (!g_raw_sockets[i]) {
            g_raw_sockets[i] = s;
            s->registered = true;
            irq_restore(irq);
            return s;
        }
    }
    irq_restore(irq);
    release(s);
    return NULL;
}

void socket_destroy_raw(socket_impl_t sh) {
    raw_socket_t* s = (raw_socket_t*)sh;
    if (!s) return;

    if (s->registered) {
        irq_flags_t irq = irq_save_disable();
        for (int i = 0; i < RAW_SOCKET_MAX; i++) if (g_raw_sockets[i] == s) {
                g_raw_sockets[i] = NULL;
                break;
        }
        s->registered = false;
        irq_restore(irq);
    }

    for (int i = 0; i < RAW_RX_RING_CAP; i++) if (s->rx[i].data) release(s->rx[i].data);
    release(s);
}

int32_t socket_close_raw(socket_impl_t sh) {
    raw_socket_t* s = (raw_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;

    if (s->registered) {
        irq_flags_t irq = irq_save_disable();
        for (int i = 0; i < RAW_SOCKET_MAX; i++) {
            if (g_raw_sockets[i] == s) {
                g_raw_sockets[i] = NULL;
                break;
            }
        }
        s->registered = false;
        irq_restore(irq);
    }

    return SOCK_OK;
}

int32_t socket_setopt_raw(socket_impl_t sh, int32_t opt, const void* value, uint32_t len) {
    raw_socket_t* s = (raw_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;

    switch ((uint32_t)opt) {
        case SOCK_OPT_RECV_TIMEOUT:
        case SOCK_OPT_DEBUG:
        case SOCK_OPT_DONTFRAG:
        case SOCK_OPT_TTL:
        case SOCK_OPT_NONBLOCK:
        case SOCK_OPT_DONTROUTE:
            return socket_common_options_set(&s->options, opt, value, len);
        case SOCK_OPT_FILTER:
            return raw_set_filter(s, value, len);
        case SOCK_OPT_SPECIAL:
            return SOCK_ERR_UNSUP;
        default:
            return SOCK_ERR_INVAL;
    }
}

int32_t socket_getopt_raw(socket_impl_t sh, int32_t opt, void* value, uint32_t* len) {
    raw_socket_t* s = (raw_socket_t*)sh;
    if (!s || !len) return SOCK_ERR_INVAL;

    switch ((uint32_t)opt) {
        case SOCK_GET_REMOTE_ENDPOINT:
            return socket_common_get_value(&s->remote_ep, sizeof(s->remote_ep), value, len);
        case SOCK_GET_BIND_SPEC:
            return socket_common_get_value(&s->bind_spec, sizeof(s->bind_spec), value, len);
        case SOCK_GET_LAST_RX_SPEC:
            return socket_common_get_value(&s->last_rx_spec, sizeof(s->last_rx_spec), value, len);
        case SOCK_GET_OPT_FILTER:
            return socket_common_get_value(&s->options.raw_filter, sizeof(s->options.raw_filter), value, len);
        default:
            break;
    }

    uint32_t v = 0;
    switch ((uint32_t)opt) {
        case SOCK_GET_BOUND:
            v = s->bound;
            break;
        case SOCK_GET_CONNECTED:
            v = s->connected;
            break;
        case SOCK_GET_RECV_QUEUED:
            v = s->rx_bytes;
            break;
        case SOCK_GET_LISTENING:
        case SOCK_GET_LOCAL_PORT:
        case SOCK_GET_SEND_QUEUED:
        case SOCK_GET_OPT_KEEPALIVE:
        case SOCK_GET_OPT_KEEPALIVE_INTERVAL:
        case SOCK_GET_OPT_TCP_NO_DELAY:
        case SOCK_GET_OPT_SEND_BUF_SIZE:
        case SOCK_GET_TCP_STATE:
        case SOCK_GET_TCP_MSS:
        case SOCK_GET_TCP_RTT_MS:
        case SOCK_GET_TCP_RETRANSMITS:
        case SOCK_GET_TCP_URGENT_REMAINING:
        case SOCK_GET_MCAST_GROUPS:
            return SOCK_ERR_UNSUP;
        case SOCK_GET_OPT_RECV_TIMEOUT:
        case SOCK_GET_OPT_DEBUG:
        case SOCK_GET_OPT_DONTFRAG:
        case SOCK_GET_OPT_TTL:
        case SOCK_GET_OPT_NONBLOCK:
        case SOCK_GET_OPT_DONTROUTE:
            return socket_common_options_get(&s->options, opt, value, len);
        case SOCK_GET_OPT_SEND_TIMEOUT:
        case SOCK_GET_OPT_BUF_SIZE:
        case SOCK_GET_OPT_BROADCAST_ALLOWED:
            return SOCK_ERR_UNSUP;
        default:
            return SOCK_ERR_INVAL;
    }

    return socket_common_get_value(&v, sizeof(v), value, len);
}

int32_t socket_bind_raw(socket_impl_t sh, const SockBindSpec* spec) {
    raw_socket_t* s = (raw_socket_t*)sh;
    if (!s || !spec) return SOCK_ERR_INVAL;

    protocol_t proto = socket_core_protocol(s->ownerSocket);
    SockBindSpec next;
    memset(&next, 0, sizeof(next));

    if (spec->kind == BIND_ANY || ((proto == PROTO_ICMP || proto == PROTO_IGMP) && spec->kind == BIND_ANY4) || (proto == PROTO_ICMPV6 && spec->kind == BIND_ANY6)) {
        s->bind_spec = next;
        s->bound = false;
        memset(&s->last_rx_spec, 0, sizeof(s->last_rx_spec));
        s->last_rx_spec.kind = BIND_ANY;
        return SOCK_OK;
    }
    if (spec->kind == BIND_L2) {
        if (!l2_interface_find_by_index(spec->ifindex)) return SOCK_ERR_INVAL;
        next.kind = BIND_L2;
        next.ifindex = spec->ifindex;
    } else if (spec->kind == BIND_L3) {
        if (proto == PROTO_ICMP || proto == PROTO_IGMP) {
            if (!l3_ipv4_find_by_id(spec->l3_id)) return SOCK_ERR_INVAL;
            next.ver = IP_VER4; 
        } else if (proto == PROTO_ICMPV6) {
            if (!l3_ipv6_find_by_id(spec->l3_id)) return SOCK_ERR_INVAL;
            next.ver = IP_VER6; 
        } else return SOCK_ERR_PROTO;
        next.kind = BIND_L3;
        next.l3_id = spec->l3_id;
    } else if (spec->kind == BIND_IP) {
        if ((proto == PROTO_ICMP || proto == PROTO_IGMP) && spec->ver == IP_VER4) {
            uint32_t ip = 0;
            memcpy(&ip, spec->ip, sizeof(ip));
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_ip(ip);
            if (!v4) return SOCK_ERR_INVAL;
            next.kind = BIND_IP;
            next.ver = IP_VER4;
            next.l3_id = v4->l3_id;
            memcpy(next.ip, spec->ip, 4);
        } else if (proto == PROTO_ICMPV6 && spec->ver == IP_VER6) {
            l3_ipv6_interface_t* v6 = l3_ipv6_find_by_ip(spec->ip);
            if (!v6) return SOCK_ERR_INVAL;
            next.kind = BIND_IP;
            next.ver = IP_VER6;
            next.l3_id = v6->l3_id;
            ipv6_cpy(next.ip, spec->ip);
        } else return SOCK_ERR_INVAL;
    } else return SOCK_ERR_INVAL;

    s->bind_spec = next;
    s->bound = true;
    memset(&s->last_rx_spec, 0, sizeof(s->last_rx_spec));
    s->last_rx_spec.kind = BIND_ANY;
    return SOCK_OK;
}

int32_t socket_connect_raw(socket_impl_t sh, const net_l4_endpoint* dst) {
    raw_socket_t* s = (raw_socket_t*)sh;
    if (!s || !dst) return SOCK_ERR_INVAL;

    protocol_t proto = socket_core_protocol(s->ownerSocket);
    if (((proto == PROTO_ICMP || proto == PROTO_IGMP) && dst->ver != IP_VER4) || (proto == PROTO_ICMPV6 && dst->ver != IP_VER6)) return SOCK_ERR_INVAL;

    s->remote_ep = *dst;
    s->remote_ep.port = 0;
    s->connected = true;
    return SOCK_OK;
}

int64_t socket_send_raw(socket_impl_t sh, const void* buf, uint64_t len) {
    raw_socket_t* s = (raw_socket_t*)sh;
    if (!s || (!buf && len) || len > UINT32_MAX) return SOCK_ERR_INVAL;
    if (!s->connected) return SOCK_ERR_NOT_BOUND;
    return socket_sendto_raw(sh, &s->remote_ep, buf, len);
}

int64_t socket_sendto_raw(socket_impl_t sh, const net_l4_endpoint* dst, const void* buf, uint64_t len) {
    raw_socket_t* s = (raw_socket_t*)sh;
    if (!s || !dst || (!buf && len) || !len || len > NETPKT_MAX_ALLOC) return SOCK_ERR_INVAL;

    protocol_t proto = socket_core_protocol(s->ownerSocket);
    if (((proto == PROTO_ICMP || proto == PROTO_IGMP) && dst->ver != IP_VER4) || (proto == PROTO_ICMPV6 && dst->ver != IP_VER6)) return SOCK_ERR_INVAL;

    ip_tx_opts_t tx;
    ip_tx_opts_t* txp = NULL;
    if (s->bound && s->bind_spec.kind == BIND_L2) {
        tx.scope = IP_TX_BOUND_L2;
        tx.target.ifindex = s->bind_spec.ifindex;
        txp = &tx;
    } else if (s->bound && (s->bind_spec.kind == BIND_L3 || s->bind_spec.kind == BIND_IP)) {
        tx.scope = IP_TX_BOUND_L3;
        tx.target.l3_id = s->bind_spec.l3_id;
        txp = &tx;
    }

    uint32_t headroom = (uint32_t)sizeof(eth_hdr_t) + ((proto == PROTO_ICMP || proto == PROTO_IGMP) ? (uint32_t)sizeof(ipv4_hdr_t) : (uint32_t)sizeof(ipv6_hdr_t));
    netpkt_t* pkt = netpkt_alloc((uint32_t)len, headroom, 0);
    if (!pkt) return SOCK_ERR_SYS;

    void* p = netpkt_put(pkt, (uint32_t)len);
    if (!p) {
        netpkt_unref(pkt);
        return SOCK_ERR_SYS;
    }
    memcpy(p, buf, (uint32_t)len);

    uint8_t ttl = (s->options.flags & SOCK_OPT_TTL) ? s->options.ttl : 0;
    uint8_t dontfrag = (s->options.flags & SOCK_OPT_DONTFRAG) ? 1 : 0;

    if (proto == PROTO_ICMP || proto == PROTO_IGMP) {
        uint32_t dst_ip = 0;
        memcpy(&dst_ip, dst->ip, sizeof(dst_ip));
        if (s->options.flags & SOCK_OPT_DONTROUTE) {
            ipv4_tx_plan_t plan;
            if (!socket_bind_build_ipv4_tx_plan(&s->bind_spec, s->bound, dst_ip, &plan) || !ipv4_tx_plan_onlink(&plan, dst_ip)) {
                netpkt_unref(pkt);
                return SOCK_ERR_NO_ROUTE;
            }
        }
        return ipv4_send_packet(dst_ip, (uint8_t)proto, pkt, txp, ttl, dontfrag) ? (int64_t)len : SOCK_ERR_SYS;
    }

    ipv6_tx_plan_t plan;
    if (!ipv6_build_tx_plan(dst->ip, txp, &plan)) {
        netpkt_unref(pkt);
        return (s->options.flags & SOCK_OPT_DONTROUTE) ? SOCK_ERR_NO_ROUTE : SOCK_ERR_SYS;
    }

    if ((s->options.flags & SOCK_OPT_DONTROUTE) && !ipv6_tx_plan_onlink(&plan, dst->ip)){
        netpkt_unref(pkt);
        return SOCK_ERR_NO_ROUTE;
    }

    if (len >= 4) {
        memset((uint8_t*)p + 2, 0, sizeof(uint16_t));
        uint16_t sum = bswap16(checksum16_pipv6(plan.src_ip, dst->ip, PROTO_ICMPV6, p, (uint32_t)len));
        memcpy((uint8_t*)p + 2, &sum, sizeof(sum));
    }

    return ipv6_send_packet(dst->ip, PROTO_ICMPV6, pkt, txp, ttl ? ttl : 64, dontfrag) ? (int64_t)len : SOCK_ERR_SYS;
}

int64_t socket_recv_raw(socket_impl_t sh, void* buf, uint64_t len, net_l4_endpoint* out_src) {
    raw_socket_t* s = (raw_socket_t*)sh;
    if (!s || (!buf && len) || len > UINT32_MAX) return SOCK_ERR_INVAL;

    for (;;) {
        irq_flags_t irq = irq_save_disable();
        bool ready = s->rx_count != 0;
        irq_restore(irq);
        if (ready) break;
        if (s->options.flags & SOCK_OPT_NONBLOCK) return SOCK_ERR_WOULDBLOCK;

        uint32_t start_ms = (uint32_t)get_time();
        while (1) {
            irq = irq_save_disable();
            ready = s->rx_count != 0;
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
    uint8_t pos = s->rx_head;
    uint8_t* data = s->rx[pos].data;
    uint32_t pkt_len = s->rx[pos].len;
    net_l4_endpoint src = s->rx[pos].src;
    s->last_rx_spec = s->rx[pos].rx_spec;
    s->rx_bytes -= pkt_len;
    s->rx[pos].data = NULL;
    s->rx[pos].len = 0;
    s->rx_head = (uint8_t)((s->rx_head + 1) % RAW_RX_RING_CAP);
    s->rx_count--;
    irq_restore(irq);

    uint32_t n = pkt_len;
    if (n > len) n = (uint32_t)len;
    if (n) memcpy(buf, data, n);
    if (out_src) *out_src = src;
    release(data);
    return n;
}

bool socket_raw_input_v4(protocol_t protocol, uint8_t ifindex, uint32_t src, uint32_t dst, netpkt_t* pkt) {
    if ((protocol != PROTO_ICMP && protocol != PROTO_IGMP) || !ifindex || !pkt) return false;

    net_l4_endpoint src_ep;
    make_ep(&src, 0, IP_VER4, &src_ep);

    SockBindSpec rx_spec;
    memset(&rx_spec, 0, sizeof(rx_spec));
    l3_ipv4_interface_t* rx_l3 = l3_ipv4_find_by_ip(dst);
    if (rx_l3) {
        rx_spec.kind = BIND_L3;
        rx_spec.ver = IP_VER4;
        rx_spec.l3_id = rx_l3->l3_id;
    } else {
        rx_spec.kind = BIND_L2;
        rx_spec.ifindex = ifindex;
    }

    raw_socket_t* targets[RAW_SOCKET_MAX];
    int n = 0;
    irq_flags_t irq = irq_save_disable();
    for (int i = 0; i < RAW_SOCKET_MAX; i++) {
        raw_socket_t* s = g_raw_sockets[i];
        if (!s || socket_core_protocol(s->ownerSocket) != protocol || socket_core_is_closing(s->ownerSocket)) continue;
        if (s->connected) {
            uint32_t remote = 0;
            memcpy(&remote, s->remote_ep.ip, sizeof(remote));
            if (remote != src) continue;
        }
        if (s->bound && s->bind_spec.kind == BIND_L2 && s->bind_spec.ifindex != ifindex) continue;
        if (s->bound && s->bind_spec.kind == BIND_L3) {
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(s->bind_spec.l3_id);
            if (!v4 || !v4->l2 || v4->l2->ifindex != ifindex) continue;
            if (!ipv4_is_multicast(dst) && dst != IPV4_LIMITED_BROADCAST && (!v4->mask || ipv4_broadcast_calc(v4->ip, v4->mask) != dst) && v4->ip != dst) continue;
        }
        if (s->bound && s->bind_spec.kind == BIND_IP) {
            uint32_t local = 0;
            memcpy(&local, s->bind_spec.ip, sizeof(local));
            if (local != dst) continue;
        }
        socket_core_ref(s->ownerSocket);
        targets[n++] = s;
    }
    irq_restore(irq);

    bool delivered = false;
    uint32_t len = netpkt_len(pkt);
    for (int i = 0; i < n; i++) {
        raw_socket_t* s = targets[i];
        if (raw_enqueue(s, &src_ep, &rx_spec, pkt, len)) delivered = true;
        socket_core_put(s->ownerSocket);
    }
    return delivered;
}

bool socket_raw_input_v6(uint8_t ifindex, const uint8_t src[16], const uint8_t dst[16], netpkt_t* pkt) {
    if (!ifindex || !src || !dst || !pkt) return false;

    net_l4_endpoint src_ep;
    make_ep(src, 0, IP_VER6, &src_ep);

    SockBindSpec rx_spec;
    memset(&rx_spec, 0, sizeof(rx_spec));
    l3_ipv6_interface_t* rx_l3 = l3_ipv6_find_by_ip(dst);
    if (rx_l3) {
        rx_spec.kind = BIND_L3;
        rx_spec.ver = IP_VER6;
        rx_spec.l3_id = rx_l3->l3_id;
    } else {
        rx_spec.kind = BIND_L2;
        rx_spec.ifindex = ifindex;
    }

    raw_socket_t* targets[RAW_SOCKET_MAX];
    int n = 0;
    irq_flags_t irq = irq_save_disable();
    for (int i = 0; i < RAW_SOCKET_MAX; i++) {
        raw_socket_t* s = g_raw_sockets[i];
        if (!s || socket_core_protocol(s->ownerSocket) != PROTO_ICMPV6 || socket_core_is_closing(s->ownerSocket)) continue;
        if (s->connected && ipv6_cmp(s->remote_ep.ip, src) != 0) continue;
        if (s->bound && s->bind_spec.kind == BIND_L2 && s->bind_spec.ifindex != ifindex) continue;
        if (s->bound && s->bind_spec.kind == BIND_L3) {
            l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(s->bind_spec.l3_id);
            if (!v6 || !v6->l2 || v6->l2->ifindex != ifindex) continue;
            if (!ipv6_is_multicast(dst) && ipv6_cmp(v6->ip, dst) != 0) continue;
        }
        if (s->bound && s->bind_spec.kind == BIND_IP && ipv6_cmp(s->bind_spec.ip, dst) != 0) continue;
        socket_core_ref(s->ownerSocket);
        targets[n++] = s;
    }
    irq_restore(irq);

    bool delivered = false;
    uint32_t len = netpkt_len(pkt);
    for (int i = 0; i < n; i++) {
        raw_socket_t* s = targets[i];
        if (raw_enqueue(s, &src_ep, &rx_spec, pkt, len)) delivered = true;
        socket_core_put(s->ownerSocket);
    }
    return delivered;
}
