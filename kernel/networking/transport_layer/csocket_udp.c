#include "csocket_udp.h"
#include "exceptions/irq.h"
#include "networking/transport_layer/socket_bind.h"
#include "networking/transport_layer/udp.h"
#include "networking/internet_layer/ipv4_route.h"
#include "networking/internet_layer/ipv6_route.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/network.h"
#include "networking/firewall.h"
#include "networking/interface_manager.h"
#include "networking/net_logger/net_logger.h"
#include "syscalls/syscalls.h"
#include "std/memory.h"
#include "alloc/allocate.h"
#define UDP_DEFAULT_RING_CAP 64
#define UDP_REPLY_WINDOW_MS 60000
#define UDP_RECENT_TX_COUNT 8
#define UDP_MAX_RING_CAP 1024

//what if MAX_L2_INTERFACES is larger than sizeof u16?
typedef struct udp_rx_entry {
    netpkt_t* pkt;
    net_l4_endpoint src;
    SockBindSpec rx_spec;
} udp_rx_entry_t;

typedef struct udp_recent_tx {
    net_l4_endpoint remote;
    uint32_t sent_at_ms;
    bool match_any_source;
} udp_recent_tx_t;

typedef struct udp_socket {
    uint16_t localPort;
    net_l4_endpoint remoteEP;
    bool connected;
    udp_recent_tx_t recent_tx[UDP_RECENT_TX_COUNT];
    uint8_t recent_tx_next;
    ksocket_t* ownerSocket;
    SocketOptions options;
    SockBindSpec bindSpec;
    SockBindSpec lastRxSpec;
    socket_bind_token_t bindToken;
    udp_rx_entry_t* rx_ring;
    uint16_t* mcast_ifmasks;
    uint32_t ring_cap;
    uint32_t r_head;
    uint32_t r_tail;
    uint32_t rx_bytes;
    bool closed;
} udp_socket_t;

static void udp_record_recent_tx(udp_socket_t* s, const net_l4_endpoint* remote, bool match_any_source) {
    if (!s || !remote) return;
    uint32_t now_ms = (uint32_t)get_time();
    irq_flags_t irq = irq_save_disable();
    uint8_t index = s->recent_tx_next;
    if (++s->recent_tx_next == UDP_RECENT_TX_COUNT) s->recent_tx_next = 0;
    s->recent_tx[index] = (udp_recent_tx_t) {.remote = *remote, .sent_at_ms = now_ms, .match_any_source = match_any_source};
    irq_restore(irq);
}

static bool udp_socket_mcast_endpoint_valid(const net_l4_endpoint* ep) {
    if (!ep) return false;
    if (ep->ver == IP_VER4) {
        uint32_t ip = 0;
        memcpy(&ip, ep->ip, 4);
        return ipv4_is_multicast(ip);
    }
    if (ep->ver == IP_VER6) return ipv6_is_multicast(ep->ip);
    return false;
}

static bool udp_socket_mcast_match(udp_socket_t* s, ip_version_t ver, const void* dst_ip_addr) {
    if (!s || !dst_ip_addr || !s->options.mcast_groups || !s->options.mcast_count) return false;

    for (uint32_t i = 0; i < s->options.mcast_count; ++i) {
        const net_l4_endpoint* group = &s->options.mcast_groups[i];
        if (group->ver != ver) continue;

        if (ver == IP_VER4) {
            uint32_t want = 0;
            uint32_t got = 0;
            memcpy(&want, group->ip, 4);
            memcpy(&got, dst_ip_addr, 4);
            if (want == got) return true;
        } else if (ver == IP_VER6 && ipv6_cmp(group->ip, dst_ip_addr) == 0) return true;
    }

    return false;
}

static int32_t udp_socket_apply_mcast_group(const SockBindSpec* spec, const net_l4_endpoint* group, bool joining, uint16_t* ifmask) {
    if (!spec || !group || !ifmask) return SOCK_ERR_INVAL;
    if (joining && *ifmask) return SOCK_ERR_STATE;

    uint32_t v4_group = 0;
    if (group->ver == IP_VER4) {
        memcpy(&v4_group, group->ip, 4);
        if (!ipv4_is_multicast(v4_group)) return SOCK_ERR_INVAL;
    } else if (group->ver == IP_VER6) {
        if (!ipv6_is_multicast(group->ip)) return SOCK_ERR_INVAL;
    } else return SOCK_ERR_INVAL;

    if (!joining) {
        uint16_t mask = *ifmask;
        for (uint8_t ifindex = 1; ifindex <= MAX_L2_INTERFACES; ++ifindex) {
            if (!(mask & (uint16_t)(1u << (ifindex - 1)))) continue;
            if (group->ver == IP_VER4) l2_ipv4_mcast_leave(ifindex, v4_group);
            else l2_ipv6_mcast_leave(ifindex, group->ip);
        }
        *ifmask = 0;
        return SOCK_OK;
    }

    uint8_t l3_ids[MAX_L3_INTERFACES];
    bool linkscope = group->ver == IP_VER6 && ipv6_is_linkscope_mcast(group->ip);
    uint32_t l3_count = socket_bind_select_l3(spec, group->ver, l3_ids, MAX_L3_INTERFACES);
    uint16_t targets = 0;
    *ifmask = 0;

    for (uint32_t i = 0; i < l3_count; ++i) {
        l2_interface_t* l2 = NULL;
        if (group->ver == IP_VER4) {
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(l3_ids[i]);
            if (!v4 || !v4->l2 || !ipv4_l3_is_active(v4)) continue;
            l2 = v4->l2;
        } else {
            l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(l3_ids[i]);
            if (!v6 || !v6->l2 || !ipv6_l3_is_ready(v6)) continue;
            if (linkscope && !ipv6_is_linklocal(v6->ip)) continue;
            l2 = v6->l2;
        }

        if (!l2->ifindex || l2->ifindex > MAX_L2_INTERFACES) continue;
        targets |= (uint16_t)(1u << (l2->ifindex - 1));
    }

    if (!targets) return SOCK_ERR_SYS;
    for (uint8_t ifindex = 1; ifindex <= MAX_L2_INTERFACES; ++ifindex) {
        uint16_t bit = (uint16_t)(1u << (ifindex - 1));
        if (!(targets & bit)) continue;

        bool ok = group->ver == IP_VER4 ? l2_ipv4_mcast_join(ifindex, v4_group) : l2_ipv6_mcast_join(ifindex, group->ip);
        if (ok) {
            *ifmask |= bit;
            continue;
        }

        uint16_t joined = *ifmask;
        for (uint8_t joined_ifindex = 1; joined_ifindex <= MAX_L2_INTERFACES; ++joined_ifindex) {
            if (!(joined & (uint16_t)(1u << (joined_ifindex - 1)))) continue;
            if (group->ver == IP_VER4) l2_ipv4_mcast_leave(joined_ifindex, v4_group);
            else l2_ipv6_mcast_leave(joined_ifindex, group->ip);
        }
        *ifmask = 0;
        return SOCK_ERR_SYS;
    }

    return SOCK_OK;
}

static int32_t udp_socket_join_mcast_groups(udp_socket_t* s, const SockBindSpec* spec) {
    if (!s || !spec || !s->options.mcast_groups || !s->options.mcast_count) return SOCK_OK;
    if (!s->mcast_ifmasks) return SOCK_ERR_SYS;

    for (uint32_t i = 0; i < s->options.mcast_count; ++i) {
        int32_t rc = udp_socket_apply_mcast_group(spec, &s->options.mcast_groups[i], true, &s->mcast_ifmasks[i]);
        if (rc == SOCK_OK) continue;

        while (i > 0) {
            --i;
            (void)udp_socket_apply_mcast_group(spec, &s->options.mcast_groups[i], false, &s->mcast_ifmasks[i]);
        }
        return rc;
    }

    return SOCK_OK;
}

static int32_t udp_socket_bind_l3(udp_socket_t* s, uint8_t l3_id) {
    if (!s || !s->ownerSocket || !l3_id) return SOCK_ERR_INVAL;
    if (s->closed) return SOCK_ERR_STATE;
    if (s->localPort) return SOCK_OK;

    SockBindSpec spec = {0};
    socket_bind_token_t token = 0;
    int32_t port = socket_bind_alloc_ephemeral_l3(s->ownerSocket, PROTO_UDP, l3_id, s->options.flags & (SOCK_OPT_REUSEADDR | SOCK_OPT_REUSEPORT), &spec, &token);
    if (port < 0) return SOCK_ERR_NO_PORT;

    int32_t rc = udp_socket_join_mcast_groups(s, &spec);
    if (rc != SOCK_OK) {
        socket_bind_remove(token);
        return rc;
    }

    if (s->connected) socket_bind_udp_set_remote(token, &s->remoteEP);

    irq_flags_t irq = irq_save_disable();
    if (s->closed) {
        irq_restore(irq);
        socket_bind_remove(token);
        return SOCK_ERR_STATE;
    }
    s->localPort = (uint16_t)port;
    s->bindToken = token;
    s->bindSpec = spec;
    irq_restore(irq);
    return SOCK_OK;
}

uint32_t socket_udp_input(ksocket_t* socket, ip_version_t ipver, uint8_t l3_id, const void* src_ip_addr, const void* dst_ip_addr, netpkt_t* pkt, uint16_t src_port, uint16_t dst_port) {
    if (!socket || !pkt || !src_ip_addr || !dst_ip_addr) return 0;
    if (ipver != IP_VER4 && ipver != IP_VER6) return 0;

    udp_socket_t* s = (udp_socket_t*)socket_core_impl(socket);
    if (!s) return 0;

    uint32_t pkt_len = netpkt_len(pkt);
    uint32_t limit = UINT32_MAX;
    if ((s->options.flags & SOCK_OPT_BUF_SIZE) && s->options.buf_size) limit = s->options.buf_size;
    if (pkt_len > limit) return 0;

    bool multicast = false;
    if (ipver == IP_VER4) {
        uint32_t dip = 0;
        memcpy(&dip, dst_ip_addr, 4);
        multicast = ipv4_is_multicast(dip);
    } else multicast = ipv6_is_multicast(dst_ip_addr);

    udp_rx_entry_t entry = {0};
    entry.pkt = pkt;
    make_ep(src_ip_addr, src_port, ipver, &entry.src);
    entry.rx_spec.kind = BIND_L3;
    entry.rx_spec.ver = ipver;
    entry.rx_spec.l3_id = l3_id;

    irq_flags_t irq = irq_save_disable();
    if (s->closed || s->localPort != dst_port) {
        irq_restore(irq);
        return 0;
    }
    bool joined_multicast = multicast && udp_socket_mcast_match(s, ipver, dst_ip_addr);
    if (multicast && !joined_multicast) {
        irq_restore(irq);
        return 0;
    }
    if (s->connected && (s->remoteEP.ver != ipver || s->remoteEP.port != src_port || (ipver == IP_VER4 && memcmp(s->remoteEP.ip, src_ip_addr, 4) != 0) || (ipver == IP_VER6 && ipv6_cmp(s->remoteEP.ip, src_ip_addr) != 0))) {
        irq_restore(irq);
        return 0;
    }
    bool recent_tx_match = false;
    uint32_t recent_tx_time_ms = 0;
    uint8_t index = s->recent_tx_next;
    for (uint32_t i = 0; i < UDP_RECENT_TX_COUNT; i++) {
        if (!index) index = UDP_RECENT_TX_COUNT;
        udp_recent_tx_t* tx = &s->recent_tx[--index];
        if (!tx->remote.port || tx->remote.ver != ipver || tx->remote.port != src_port) continue;
        if (tx->match_any_source || (ipver == IP_VER4 && memcmp(tx->remote.ip, src_ip_addr, 4) == 0) || (ipver == IP_VER6 && ipv6_cmp(tx->remote.ip, src_ip_addr) == 0)) {
            recent_tx_match = true;
            recent_tx_time_ms = tx->sent_at_ms;
            break;
        }
    }
    irq_restore(irq);

    bool related = recent_tx_match && (uint32_t)get_time() - recent_tx_time_ms <= UDP_REPLY_WINDOW_MS;
    if (!firewall_allows(PROTO_UDP, NET_CTRL_FIREWALL_IN, &entry.src, dst_port, related)) return 0;

    if (!s->rx_ring || !s->ring_cap) {
        uint32_t usable = UDP_DEFAULT_RING_CAP;
        if ((s->options.flags & SOCK_OPT_BUF_SIZE) && s->options.buf_size) {
            usable = s->options.buf_size / MAX_PACKET_SIZE;
            if (usable < 4) usable = 4;
            if (usable > UDP_MAX_RING_CAP) usable = UDP_MAX_RING_CAP;
        }

        udp_rx_entry_t* ring = (udp_rx_entry_t*)zalloc(sizeof(udp_rx_entry_t) * (usable+1));
        if (!ring) return 0;
        irq_flags_t irq = irq_save_disable();
        if (!s->closed && !s->rx_ring) {
            s->rx_ring = ring;
            s->ring_cap = usable + 1;
            ring = NULL;
        }
        irq_restore(irq);
        if (ring) release(ring);
    }

    irq = irq_save_disable();
    if (s->closed || s->localPort != dst_port || !s->rx_ring || !s->ring_cap) {
        irq_restore(irq);
        return 0;
    }

    if (multicast && !udp_socket_mcast_match(s, ipver, dst_ip_addr)) {
        irq_restore(irq);
        return 0;
    }

    if (s->connected && (s->remoteEP.ver != ipver || s->remoteEP.port != src_port || (ipver == IP_VER4 && memcmp(s->remoteEP.ip, src_ip_addr, 4) != 0) || (ipver == IP_VER6 && ipv6_cmp(s->remoteEP.ip, src_ip_addr) != 0))) {
        irq_restore(irq);
        return 0;
    }

    if (s->rx_bytes > limit - pkt_len) {
        irq_restore(irq);
        return 0;
    }

    uint32_t nexti = (s->r_tail + 1) % s->ring_cap;
    if (nexti == s->r_head) {
        irq_restore(irq);
        return 0;
    }

    netpkt_ref(pkt);
    s->rx_ring[s->r_tail] = entry;
    s->rx_bytes += pkt_len;
    s->r_tail = nexti;
    irq_restore(irq);
    return pkt_len;
}

socket_impl_t udp_socket_create(ksocket_t* owner, const SocketOptions* extra) {
    if (!owner) return NULL;

    uint32_t supported = SOCK_OPT_RECV_TIMEOUT | SOCK_OPT_BUF_SIZE | SOCK_OPT_DEBUG | SOCK_OPT_DONTFRAG | SOCK_OPT_BROADCAST_ALLOWED | SOCK_OPT_TTL | SOCK_OPT_MCAST_JOIN | SOCK_OPT_NONBLOCK | SOCK_OPT_DONTROUTE | SOCK_OPT_REUSEADDR | SOCK_OPT_REUSEPORT;
    if (extra) {
        if (extra->flags & ~supported) return NULL;
        if ((extra->flags & SOCK_OPT_DEBUG) && extra->debug_level > SOCK_DBG_ALL) return NULL;
        if ((extra->flags & SOCK_OPT_BUF_SIZE) && !extra->buf_size) return NULL;
    }

    udp_socket_t* s = (udp_socket_t*)zalloc(sizeof(*s));
    if (!s) return NULL;
    s->ownerSocket = owner;
    s->remoteEP.ver = IP_VER4;
    s->bindSpec.kind = BIND_ANY;
    s->lastRxSpec.kind = BIND_ANY;
    if (extra) s->options = *extra;
    s->options.flags &= ~(SOCK_OPT_MCAST_JOIN | SOCK_OPT_MCAST_LEAVE);
    s->options.mcast_count = 0;
    s->options.mcast_groups = NULL;

    if (extra && extra->mcast_count) {
        if (!extra->mcast_groups || socket_setopt_udp(s, SOCK_OPT_MCAST_JOIN, extra->mcast_groups, sizeof(net_l4_endpoint) * extra->mcast_count) != SOCK_OK) {
            socket_destroy_udp(s);
            return NULL;
        }
    }

    return s;
}

int32_t socket_bind_udp(socket_impl_t sh, const SockBindSpec* spec_in, uint16_t port) {
    udp_socket_t* s = (udp_socket_t*)sh;
    if (!s || !spec_in) return SOCK_ERR_INVAL;

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_UDP;
    ev.action = NETLOG_ACT_BIND;
    ev.pid = socket_core_pid(s->ownerSocket);
    ev.u0 = port;
    ev.bind_spec = *spec_in;
    netlog_socket_event(&s->options, &ev);

    if (s->closed) return SOCK_ERR_STATE;
    if (s->localPort) return SOCK_ERR_BOUND;
    if (!s->ownerSocket) return SOCK_ERR_SYS;

    SockBindSpec spec = *spec_in;
    if (!socket_bind_prepare_spec(&spec, PROTO_UDP)) return SOCK_ERR_INVAL;

    int32_t bind_port = port;
    socket_bind_token_t token = 0;
    if (bind_port == 0) {
        bind_port = socket_bind_alloc_ephemeral(s->ownerSocket, PROTO_UDP, &spec, s->options.flags & (SOCK_OPT_REUSEADDR | SOCK_OPT_REUSEPORT), &token);
        if (bind_port < 0) return SOCK_ERR_NO_PORT;
    } else if (!socket_bind_insert(s->ownerSocket, PROTO_UDP, &spec, port, s->options.flags & (SOCK_OPT_REUSEADDR | SOCK_OPT_REUSEPORT), true, &token)) return SOCK_ERR_BOUND;

    int32_t rc = udp_socket_join_mcast_groups(s, &spec);
    if (rc != SOCK_OK) {
        socket_bind_remove(token);
        return rc;
    }

    if (s->connected) socket_bind_udp_set_remote(token, &s->remoteEP);

    irq_flags_t irq = irq_save_disable();
    if (s->closed) {
        irq_restore(irq);
        socket_bind_remove(token);
        return SOCK_ERR_STATE;
    }
    s->bindSpec = spec;
    s->bindToken = token;
    s->localPort = (uint16_t)bind_port;
    irq_restore(irq);
    return SOCK_OK;
}

int32_t socket_connect_udp(socket_impl_t sh, const net_l4_endpoint* dst) {
    udp_socket_t* s = (udp_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_UDP;
    ev.action = NETLOG_ACT_CONNECT;
    ev.pid = socket_core_pid(s->ownerSocket);
    if (dst) ev.dst_ep = *dst;
    netlog_socket_event(&s->options, &ev);

    if (s->closed) return SOCK_ERR_STATE;
    if (!dst || !dst->port) return SOCK_ERR_INVAL;
    if (dst->ver != IP_VER4 && dst->ver != IP_VER6) return SOCK_ERR_INVAL;

    if (dst->ver == IP_VER4) {
        uint32_t dip = 0;
        memcpy(&dip, dst->ip, 4);
        ipv4_tx_plan_t plan;
        if (!socket_bind_build_ipv4_tx_plan(&s->bindSpec, s->localPort != 0, dip, &plan)) return SOCK_ERR_NO_ROUTE;
        if ((s->options.flags & SOCK_OPT_DONTROUTE) && !ipv4_tx_plan_onlink(&plan, dip)) return SOCK_ERR_NO_ROUTE;
    } else {
        ipv6_tx_plan_t plan;
        if (!socket_bind_build_ipv6_tx_plan(&s->bindSpec, s->localPort != 0, dst->ip, &plan)) return SOCK_ERR_NO_ROUTE;
        if ((s->options.flags & SOCK_OPT_DONTROUTE) && !ipv6_tx_plan_onlink(&plan, dst->ip)) return SOCK_ERR_NO_ROUTE;
    }

    if (s->bindToken) socket_bind_udp_set_remote(s->bindToken, dst);
    irq_flags_t irq = irq_save_disable();
    if (s->closed) {
        irq_restore(irq);
        return SOCK_ERR_STATE;
    }
    s->remoteEP = *dst;
    s->connected = true;
    irq_restore(irq);
    return SOCK_OK;
}

int64_t socket_sendto_udp(socket_impl_t sh, const net_l4_endpoint* dst, const void* buf, uint64_t len) {
    udp_socket_t* s = (udp_socket_t*)sh;
    if (!s || (!buf && len)) return SOCK_ERR_INVAL;
    if (s->closed) return SOCK_ERR_STATE;

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_UDP;
    ev.action = NETLOG_ACT_SENDTO;
    ev.pid = socket_core_pid(s->ownerSocket);
    ev.u1 = (uint32_t)len;
    if (dst) {
        ev.dst_ep = *dst;
        ev.u0 = dst->port;
    }
    netlog_socket_event(&s->options, &ev);

    bool explicit_dst = dst != NULL;
    if (!dst) {
        if (!s->connected) return SOCK_ERR_STATE;
        dst = &s->remoteEP;
    }
    if (!dst->port) return SOCK_ERR_INVAL;
    if (!s->ownerSocket) return SOCK_ERR_SYS;

    net_l4_endpoint d = *dst;
    if (d.ver != IP_VER4 && d.ver != IP_VER6) return SOCK_ERR_INVAL;
    uint64_t max_payload = d.ver == IP_VER4 ? UINT16_MAX - sizeof(ipv4_hdr_t) - sizeof(udp_hdr_t) : UINT16_MAX - sizeof(udp_hdr_t);
    if (len > max_payload) return SOCK_ERR_INVAL;
    if (s->connected && explicit_dst) {
        if (d.ver != s->remoteEP.ver || d.port != s->remoteEP.port) return SOCK_ERR_STATE;
        if (d.ver == IP_VER4 && memcmp(d.ip, s->remoteEP.ip, 4) != 0) return SOCK_ERR_STATE;
        if (d.ver == IP_VER6 && ipv6_cmp(d.ip, s->remoteEP.ip) != 0) return SOCK_ERR_STATE;
    }
    if (!firewall_allows(PROTO_UDP, NET_CTRL_FIREWALL_OUT, &d, s->localPort, false)) return SOCK_ERR_PERM;

    sizedptr pay;
    pay.ptr = (uintptr_t)buf;
    pay.size = (uint32_t)len;
    uint8_t ttl = (s->options.flags & SOCK_OPT_TTL) ? s->options.ttl : 0;
    uint8_t dontfrag = (s->options.flags & SOCK_OPT_DONTFRAG) ? 1 : 0;

    if (d.ver == IP_VER4) {
        uint32_t dip = 0;
        memcpy(&dip, d.ip, 4);

        bool limited_bcast = ipv4_is_limited_broadcast(dip);
        uint8_t bcast_ids[MAX_IPV4_L3_INTERFACES];
        uint8_t chosen_l3 = 0;
        l3_ipv4_interface_t* bcast_v4 = NULL;

        if (limited_bcast) {
            if (!(s->options.flags & SOCK_OPT_BROADCAST_ALLOWED)) return SOCK_ERR_PERM;

            uint32_t n = socket_bind_select_l3(&s->bindSpec, IP_VER4, bcast_ids, MAX_IPV4_L3_INTERFACES);
            uint32_t valid = 0;
            for (uint32_t i = 0; i < n; ++i) {
                l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(bcast_ids[i]);
                if (!ipv4_l3_is_active(v4) || v4->is_localhost) continue;
                if (!v4->ip && v4->mode != IPV4_CFG_DHCP) continue;
                if (!valid) {
                    chosen_l3 = bcast_ids[i];
                    bcast_v4 = v4;
                }
                valid++;
            }
            if (valid != 1 || !bcast_v4) return SOCK_ERR_INVAL;
        } else {
            uint32_t n = socket_bind_select_l3(&s->bindSpec, IP_VER4, bcast_ids, MAX_IPV4_L3_INTERFACES);
            for (uint32_t i = 0; i < n; ++i) {
                l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(bcast_ids[i]);
                if (!ipv4_l3_is_ready(v4) || !v4->mask || v4->is_localhost) continue;
                if (ipv4_broadcast_calc(v4->ip, v4->mask) != dip) continue;
                chosen_l3 = bcast_ids[i];
                bcast_v4 = v4;
                break;
            }
            if (bcast_v4 && !(s->options.flags & SOCK_OPT_BROADCAST_ALLOWED)) return SOCK_ERR_PERM;
        }

        if (bcast_v4) {
            int32_t bind_rc = udp_socket_bind_l3(s, chosen_l3);
            if (bind_rc != SOCK_OK) return bind_rc;

            net_l4_endpoint src;
            make_ep(&bcast_v4->ip, s->localPort, IP_VER4, &src);

            ip_tx_opts_t tx;
            tx.scope = IP_TX_BOUND_L3;
            tx.index = chosen_l3;

            if (!udp_send_segment(&src, &d, pay, &tx, ttl, dontfrag)) return SOCK_ERR_SYS;
            udp_record_recent_tx(s, &d, true);
            return (int64_t)len;
        }

        ipv4_tx_plan_t plan;
        if (!socket_bind_build_ipv4_tx_plan(&s->bindSpec, s->localPort != 0, dip, &plan)) return SOCK_ERR_NO_ROUTE;
        if ((s->options.flags & SOCK_OPT_DONTROUTE) && !ipv4_tx_plan_onlink(&plan, dip)) return SOCK_ERR_NO_ROUTE;

        uint8_t tx_l3 = plan.l3_id;
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(tx_l3);
        if (!ipv4_l3_is_ready(v4)) return SOCK_ERR_NO_ROUTE;

        int32_t bind_rc = udp_socket_bind_l3(s, tx_l3);
        if (bind_rc != SOCK_OK) return bind_rc;

        net_l4_endpoint src;
        make_ep(&v4->ip, s->localPort, IP_VER4, &src);

        ip_tx_opts_t tx;
        tx.scope = IP_TX_BOUND_L3;
        tx.index = plan.l3_id;

        if (!udp_send_segment(&src, &d, pay, &tx, ttl, dontfrag)) return SOCK_ERR_SYS;
        udp_record_recent_tx(s, &d, ipv4_is_multicast(dip));
        return (int64_t)len;
    }

    if (d.ver == IP_VER6) {
        ipv6_tx_plan_t plan;
        if (!socket_bind_build_ipv6_tx_plan(&s->bindSpec, s->localPort != 0, d.ip, &plan)) return SOCK_ERR_NO_ROUTE;
        if ((s->options.flags & SOCK_OPT_DONTROUTE) && !ipv6_tx_plan_onlink(&plan, d.ip)) return SOCK_ERR_NO_ROUTE;

        uint8_t chosen_l3 = plan.l3_id;
        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(chosen_l3);
        if (!ipv6_l3_is_ready(v6)) return SOCK_ERR_NO_ROUTE;

        int32_t bind_rc = udp_socket_bind_l3(s, chosen_l3);
        if (bind_rc != SOCK_OK) return bind_rc;

        net_l4_endpoint src;
        make_ep(v6->ip, s->localPort, IP_VER6, &src);

        ip_tx_opts_t tx;
        tx.scope = IP_TX_BOUND_L3;
        tx.index = plan.l3_id;

        if (!udp_send_segment(&src, &d, pay, &tx, ttl, dontfrag)) return SOCK_ERR_SYS;
        udp_record_recent_tx(s, &d, ipv6_is_multicast(d.ip));
        return (int64_t)len;
    }

    return SOCK_ERR_INVAL;
} 

int64_t socket_recvfrom_udp(socket_impl_t sh, void* buf, uint64_t len, net_l4_endpoint* out_src) {
    udp_socket_t* s = (udp_socket_t*)sh;
    if (!s || (!buf && len)) return SOCK_ERR_INVAL;

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_UDP;
    ev.action = NETLOG_ACT_RECVFROM;
    ev.pid = socket_core_pid(s->ownerSocket);
    ev.u0 = (uint32_t)len;
    ev.local_port = s->localPort;
    ev.remote_ep = s->remoteEP;
    netlog_socket_event(&s->options, &ev);

    uint32_t start_ms = 0;
    netpkt_t* p = NULL;
    net_l4_endpoint se = {0};

    while (1){
        irq_flags_t irq = irq_save_disable();
        if (s->rx_ring && s->r_head != s->r_tail) {
            p = s->rx_ring[s->r_head].pkt;
            se = s->rx_ring[s->r_head].src;
            s->lastRxSpec = s->rx_ring[s->r_head].rx_spec;
            memset(&s->rx_ring[s->r_head], 0, sizeof(s->rx_ring[s->r_head]));
            s->r_head = (s->r_head + 1) % s->ring_cap;

            uint32_t pkt_len = p ? netpkt_len(p) : 0;
            if (s->rx_bytes >= pkt_len) s->rx_bytes -= pkt_len;
            else s->rx_bytes = 0;
            irq_restore(irq);
            break;
        }

        bool closed = s->closed;
        uint32_t flags = s->options.flags;
        uint32_t timeout_ms = s->options.recv_timeout_ms;
        irq_restore(irq);

        if (closed) return 0;
        if (flags & SOCK_OPT_NONBLOCK) return SOCK_ERR_WOULDBLOCK;
        if ((flags & SOCK_OPT_RECV_TIMEOUT) && timeout_ms) {
            uint32_t now_ms = (uint32_t)get_time();
            if (!start_ms) start_ms = now_ms;
            uint32_t elapsed_ms = now_ms - start_ms;
            if (elapsed_ms >= timeout_ms) return SOCK_ERR_WOULDBLOCK;

            uint32_t wait_ms = timeout_ms - elapsed_ms;
            if (wait_ms > 5) wait_ms = 5;
            msleep(wait_ms);
        }else msleep(5);
    }

    uint32_t pkt_len = p ? netpkt_len(p) : 0;
    uint32_t tocpy = pkt_len;
    if (tocpy > len) tocpy = (uint32_t)len;

    if (tocpy && !netpkt_copyout(p, 0, buf, tocpy)) tocpy = 0;
    if (out_src) *out_src = se;

    if (p) netpkt_unref(p);
    return tocpy;
}

int32_t socket_setopt_udp(socket_impl_t sh, int32_t opt, const void* value, uint32_t len) {
    udp_socket_t* s = (udp_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;
    if (s->closed) return SOCK_ERR_STATE;

    switch ((uint32_t)opt) {
        case SOCK_OPT_KEEPALIVE:
        case SOCK_OPT_KEEPALIVE_INTERVAL:
        case SOCK_OPT_TCP_NO_DELAY:
        case SOCK_OPT_SEND_BUF_SIZE:
        case SOCK_OPT_TCP_MAXSEG:
        case SOCK_OPT_TCP_SACK:
        case SOCK_OPT_TCP_DSACK:
        case SOCK_OPT_LINGER:
        case SOCK_OPT_FILTER:
        case SOCK_OPT_SPECIAL:
            return SOCK_ERR_UNSUP;
        case SOCK_OPT_REUSEADDR:
        case SOCK_OPT_REUSEPORT:
            if (s->localPort) return SOCK_ERR_STATE;
            break;
        case SOCK_OPT_NONBLOCK:
        case SOCK_OPT_DONTROUTE:
            break;
        case SOCK_OPT_MCAST_JOIN: {
            if (!value || !len || (len % sizeof(net_l4_endpoint)) != 0) return SOCK_ERR_INVAL;
            uint32_t count = len / sizeof(net_l4_endpoint);
            if (!count || count > 255 || count + s->options.mcast_count > 255) return SOCK_ERR_INVAL;

            const net_l4_endpoint* groups = value;
            for (uint32_t i = 0; i < count; ++i) if (!udp_socket_mcast_endpoint_valid(&groups[i])) return SOCK_ERR_INVAL;

            uint8_t old_count = s->options.mcast_count;
            if (old_count && (!s->options.mcast_groups || !s->mcast_ifmasks)) return SOCK_ERR_SYS;
            uint32_t capacity = old_count + count;
            net_l4_endpoint* next_groups = (net_l4_endpoint*)zalloc(sizeof(net_l4_endpoint) * capacity);
            uint16_t* next_ifmasks = (uint16_t*)zalloc(sizeof(uint16_t) * capacity);
            if (!next_groups || !next_ifmasks) {
                if (next_groups) release(next_groups);
                if (next_ifmasks) release(next_ifmasks);
                return SOCK_ERR_SYS;
            }

            if (old_count) {
                memcpy(next_groups, s->options.mcast_groups, sizeof(net_l4_endpoint) * old_count);
                memcpy(next_ifmasks, s->mcast_ifmasks, sizeof(uint16_t) * old_count);
            }

            uint8_t next_count = old_count;
            for (uint32_t i = 0; i < count; ++i) {
                bool exists = false;
                for (uint8_t j = 0; j < next_count; ++j) {
                    if (next_groups[j].ver != groups[i].ver) continue;
                    uint32_t ip_len = groups[i].ver == IP_VER4 ? 4 : 16;
                    if (memcmp(next_groups[j].ip, groups[i].ip, ip_len) == 0) {
                        exists = true;
                        break;
                    }
                }
                if (exists) continue;

                next_groups[next_count] = groups[i];
                next_groups[next_count].port = 0;
                if (s->localPort) {
                    int32_t rc = udp_socket_apply_mcast_group(&s->bindSpec, &next_groups[next_count], true, &next_ifmasks[next_count]);
                    if (rc != SOCK_OK) {
                        while (next_count > old_count) {
                            next_count--;
                            (void)udp_socket_apply_mcast_group(&s->bindSpec, &next_groups[next_count], false, &next_ifmasks[next_count]);
                        }
                        release(next_groups);
                        release(next_ifmasks);
                        return rc;
                    }
                }
                next_count++;
            }

            if (next_count == old_count) {
                release(next_groups);
                release(next_ifmasks);
                return SOCK_OK;
            }

            irq_flags_t irq = irq_save_disable();
            net_l4_endpoint* old_groups = (net_l4_endpoint*)s->options.mcast_groups;
            uint16_t* old_ifmasks = s->mcast_ifmasks;
            s->options.mcast_groups = next_groups;
            s->mcast_ifmasks = next_ifmasks;
            s->options.mcast_count = next_count;
            s->options.flags |= SOCK_OPT_MCAST_JOIN;
            irq_restore(irq);

            if (old_groups) release(old_groups);
            if (old_ifmasks) release(old_ifmasks);
            return SOCK_OK;
        }
        case SOCK_OPT_MCAST_LEAVE: {
            if (!value || !len || (len % sizeof(net_l4_endpoint)) != 0) return SOCK_ERR_INVAL;
            uint32_t count = len / sizeof(net_l4_endpoint);
            if (!count || count > 255) return SOCK_ERR_INVAL;
            if (!s->options.mcast_groups || !s->options.mcast_count) return SOCK_OK;

            const net_l4_endpoint* groups = value;
            for (uint32_t i = 0; i < count; ++i) if (!udp_socket_mcast_endpoint_valid(&groups[i])) return SOCK_ERR_INVAL;

            uint8_t old_count = s->options.mcast_count;
            if (!s->mcast_ifmasks) return SOCK_ERR_SYS;
            net_l4_endpoint* next_groups = (net_l4_endpoint*)zalloc(sizeof(net_l4_endpoint) * old_count);
            uint16_t* next_ifmasks = (uint16_t*)zalloc(sizeof(uint16_t) * old_count);
            if (!next_groups || !next_ifmasks) {
                if (next_groups) release(next_groups);
                if (next_ifmasks) release(next_ifmasks);
                return SOCK_ERR_SYS;
            }

            uint8_t next_count = 0;
            for (uint8_t i = 0; i < old_count; ++i) {
                bool removed = false;
                for (uint32_t j = 0; j < count; ++j) {
                    if (s->options.mcast_groups[i].ver != groups[j].ver) continue;
                    uint32_t ip_len = groups[j].ver == IP_VER4 ? 4 : 16;
                    if (memcmp(s->options.mcast_groups[i].ip, groups[j].ip, ip_len) == 0) {
                        removed = true;
                        break;
                    }
                }
                if (removed) continue;
                next_groups[next_count] = s->options.mcast_groups[i];
                next_ifmasks[next_count] = s->mcast_ifmasks[i];
                next_count++;
            }

            if (next_count == old_count) {
                release(next_groups);
                release(next_ifmasks);
                return SOCK_OK;
            }

            if (!next_count) {
                release(next_groups);
                release(next_ifmasks);
                next_groups = NULL;
                next_ifmasks = NULL;
            }

            irq_flags_t irq = irq_save_disable();
            net_l4_endpoint* old_groups = (net_l4_endpoint*)s->options.mcast_groups;
            uint16_t* old_ifmasks = s->mcast_ifmasks;
            s->options.mcast_groups = next_groups;
            s->mcast_ifmasks = next_ifmasks;
            s->options.mcast_count = next_count;
            if (next_count) s->options.flags |= SOCK_OPT_MCAST_JOIN;
            else s->options.flags &= ~SOCK_OPT_MCAST_JOIN;
            irq_restore(irq);

            if (s->localPort) {
                for (uint8_t i = 0; i < old_count; ++i) {
                    bool kept = false;
                    for (uint8_t j = 0; j < next_count; ++j) {
                        if (old_groups[i].ver != next_groups[j].ver) continue;
                        uint32_t ip_len = old_groups[i].ver == IP_VER4 ? 4 : 16;
                        if (memcmp(old_groups[i].ip, next_groups[j].ip, ip_len) == 0) {
                            kept = true;
                            break;
                        }
                    }
                    if (!kept) (void)udp_socket_apply_mcast_group(&s->bindSpec, &old_groups[i], false, &old_ifmasks[i]);
                }
            }

            release(old_groups);
            release(old_ifmasks);
            return SOCK_OK;
        }
        case SOCK_OPT_SEND_TIMEOUT:
            return SOCK_ERR_UNSUP;
        case SOCK_OPT_RECV_TIMEOUT:
        case SOCK_OPT_DEBUG:
        case SOCK_OPT_DONTFRAG:
        case SOCK_OPT_BROADCAST_ALLOWED:
        case SOCK_OPT_TTL:
            break;
        case SOCK_OPT_BUF_SIZE:
            if (s->localPort) return SOCK_ERR_STATE;
            break;
        default:
            return SOCK_ERR_INVAL;
    }

    return socket_common_options_set(&s->options, opt, value, len);
}

int32_t socket_getopt_udp(socket_impl_t sh, int32_t opt, void* value, uint32_t* len) {
    udp_socket_t* s = (udp_socket_t*)sh;
    if (!s || !len) return SOCK_ERR_INVAL;

    switch ((uint32_t)opt) {
        case SOCK_GET_REMOTE_ENDPOINT: {
            irq_flags_t irq = irq_save_disable();
            net_l4_endpoint remote = s->remoteEP;
            irq_restore(irq);
            return socket_common_get_value(&remote, sizeof(remote), value, len);
        }
        case SOCK_GET_BIND_SPEC: {
            irq_flags_t irq = irq_save_disable();
            SockBindSpec spec = s->bindSpec;
            irq_restore(irq);
            return socket_common_get_value(&spec, sizeof(spec), value, len);
        }
        case SOCK_GET_LAST_RX_SPEC: {
            irq_flags_t irq = irq_save_disable();
            SockBindSpec spec = s->lastRxSpec;
            irq_restore(irq);
            return socket_common_get_value(&spec, sizeof(spec), value, len);
        }
        case SOCK_GET_MCAST_GROUPS: {
            irq_flags_t irq = irq_save_disable();
            uint32_t need = s->options.mcast_count * sizeof(net_l4_endpoint);
            if (!value) {
                *len = need;
                irq_restore(irq);
                return SOCK_OK;
            }
            if (*len < need) {
                irq_restore(irq);
                return SOCK_ERR_INVAL;
            }
            if (need) memcpy(value, s->options.mcast_groups, need);
            *len = need;
            irq_restore(irq);
            return SOCK_OK;
        }
        case SOCK_GET_OPT_RECV_TIMEOUT:
        case SOCK_GET_OPT_BUF_SIZE:
        case SOCK_GET_OPT_DEBUG:
        case SOCK_GET_OPT_DONTFRAG:
        case SOCK_GET_OPT_BROADCAST_ALLOWED:
        case SOCK_GET_OPT_TTL:
        case SOCK_GET_OPT_NONBLOCK:
        case SOCK_GET_OPT_DONTROUTE:
        case SOCK_GET_OPT_REUSEADDR:
        case SOCK_GET_OPT_REUSEPORT:
            return socket_common_options_get(&s->options, opt, value, len);
        default:
            break;
    }

    uint32_t v = 0;
    irq_flags_t irq = irq_save_disable();
    switch ((uint32_t)opt) {
        case SOCK_GET_BOUND:
            v = s->localPort != 0;
            break;
        case SOCK_GET_CONNECTED:
            v = s->connected;
            break;
        case SOCK_GET_LISTENING:
            irq_restore(irq);
            return SOCK_ERR_UNSUP;
        case SOCK_GET_LOCAL_PORT:
            v = s->localPort;
            break;
        case SOCK_GET_RECV_QUEUED:
            v = s->rx_bytes;
            break;
        case SOCK_GET_SEND_QUEUED:
            v = 0;
            break;
        case SOCK_GET_OPT_KEEPALIVE:
        case SOCK_GET_OPT_KEEPALIVE_INTERVAL:
        case SOCK_GET_OPT_TCP_NO_DELAY:
        case SOCK_GET_OPT_SEND_BUF_SIZE:
        case SOCK_GET_OPT_TCP_MAXSEG:
        case SOCK_GET_OPT_TCP_SACK:
        case SOCK_GET_OPT_TCP_DSACK:
        case SOCK_GET_OPT_LINGER:
        case SOCK_GET_OPT_FILTER:
        case SOCK_GET_OPT_SEND_TIMEOUT:
        case SOCK_GET_TCP_STATE:
        case SOCK_GET_TCP_MSS:
        case SOCK_GET_TCP_RTT_MS:
        case SOCK_GET_TCP_RETRANSMITS:
        case SOCK_GET_TCP_URGENT_REMAINING:
            irq_restore(irq);
            return SOCK_ERR_UNSUP;
        default:
            irq_restore(irq);
            return SOCK_ERR_INVAL;
    }

    irq_restore(irq);
    return socket_common_get_value(&v, sizeof(v), value, len);
}

int32_t socket_close_udp(socket_impl_t sh) {
    udp_socket_t* s = (udp_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_UDP;
    ev.action = NETLOG_ACT_CLOSE;
    ev.pid = socket_core_pid(s->ownerSocket);

    irq_flags_t irq = irq_save_disable();
    if (s->closed) {
        irq_restore(irq);
        return SOCK_OK;
    }

    udp_rx_entry_t* rx_ring = s->rx_ring;
    uint32_t ring_cap = s->ring_cap;
    uint32_t r_head = s->r_head;
    uint32_t r_tail = s->r_tail;
    net_l4_endpoint* mcast_groups = (net_l4_endpoint*)s->options.mcast_groups;
    uint16_t* mcast_ifmasks = s->mcast_ifmasks;
    uint8_t mcast_count = s->options.mcast_count;
    socket_bind_token_t bind_token = s->bindToken;
    SockBindSpec bind_spec = s->bindSpec;
    SocketOptions options = s->options;
    ev.local_port = s->localPort;
    ev.remote_ep = s->remoteEP;

    s->closed = true;
    s->rx_ring = NULL;
    s->ring_cap = 0;
    s->r_head = 0;
    s->r_tail = 0;
    s->rx_bytes = 0;
    s->options.mcast_groups = NULL;
    s->mcast_ifmasks = NULL;
    s->options.mcast_count = 0;
    s->options.flags &= ~(SOCK_OPT_MCAST_JOIN | SOCK_OPT_MCAST_LEAVE);
    memset(&s->lastRxSpec, 0, sizeof(s->lastRxSpec));
    s->lastRxSpec.kind = BIND_ANY;
    s->bindToken = 0;
    s->localPort = 0;
    memset(&s->bindSpec, 0, sizeof(s->bindSpec));
    s->bindSpec.kind = BIND_ANY;
    s->connected = false;
    memset(s->recent_tx, 0, sizeof(s->recent_tx));
    s->recent_tx_next = 0;
    memset(&s->remoteEP, 0, sizeof(s->remoteEP));
    s->remoteEP.ver = IP_VER4;
    irq_restore(irq);

    if (bind_token) socket_bind_remove(bind_token);
    netlog_socket_event(&options, &ev);

    if (ev.local_port && mcast_groups && mcast_ifmasks) {
        for (uint32_t i = 0; i < mcast_count; ++i) (void)udp_socket_apply_mcast_group(&bind_spec, &mcast_groups[i], false, &mcast_ifmasks[i]);
    }

    if (rx_ring && ring_cap) {
        while (r_head != r_tail) {
            if (rx_ring[r_head].pkt) netpkt_unref(rx_ring[r_head].pkt);
            r_head = (r_head + 1) % ring_cap;
        }
        release(rx_ring);
    } 
    if (mcast_groups) release(mcast_groups);
    if (mcast_ifmasks) release(mcast_ifmasks);
    return SOCK_OK;
}

void socket_destroy_udp(socket_impl_t sh) {
    udp_socket_t* s = (udp_socket_t*)sh;
    if (!s) return;
    socket_close_udp(s);
    release(s);
}
