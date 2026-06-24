#include "csocket_udp.h"
#include "networking/transport_layer/socket_bind.h"
#include "networking/transport_layer/udp.h"
#include "networking/internet_layer/ipv4_route.h"
#include "networking/internet_layer/ipv6_route.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/network.h"
#include "networking/interface_manager.h"
#include "networking/net_logger/net_logger.h"
#include "syscalls/syscalls.h"
#include "std/memory.h"
#include "alloc/allocate.h"

#define UDP_DEFAULT_RING_CAP 64
#define UDP_MAX_RING_CAP 1024

typedef struct udp_rx_entry {
    netpkt_t* pkt;
    net_l4_endpoint src;
    SockBindSpec rx_spec;
} udp_rx_entry_t;

typedef struct udp_socket {
    uint16_t localPort;
    net_l4_endpoint remoteEP;
    bool connected;
    ksocket_t* ownerSocket;
    SocketOptions options;
    SockBindSpec bindSpec;
    SockBindSpec lastRxSpec;
    socket_bind_token_t bindToken;
    udp_rx_entry_t* rx_ring;
    net_l4_endpoint* mcast_groups;
    uint8_t mcast_count;
    uint32_t ring_cap;
    uint32_t r_head;
    uint32_t r_tail;
    uint32_t rx_bytes;
} udp_socket_t;

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
    if (!s || !dst_ip_addr) return false;
    if (!s->mcast_groups || !s->mcast_count) return false;

    for (uint32_t i = 0; i < s->mcast_count; ++i) {
        const net_l4_endpoint* group = &s->mcast_groups[i];
        if (group->ver != ver) continue;

        if (ver == IP_VER4) {
            uint32_t want = 0;
            uint32_t got = 0;
            memcpy(&want, group->ip, 4);
            memcpy(&got, dst_ip_addr, 4);
            if (want == got) return true;
        } else if (ver == IP_VER6) {
            if (ipv6_cmp(group->ip, dst_ip_addr) == 0) return true;
        }
    }

    return false;
}

static void udp_socket_leave_mcast_groups(udp_socket_t* s) {
    if (!s || !s->mcast_groups || !s->mcast_count || !s->localPort) return;

    uint8_t ids[MAX_L3_INTERFACES];
    for (uint32_t gidx = 0; gidx < s->mcast_count; ++gidx) {
        const net_l4_endpoint* group = &s->mcast_groups[gidx];

        if (group->ver == IP_VER4) {
            uint32_t g = 0;
            memcpy(&g, group->ip, 4);
            if (!ipv4_is_multicast(g)) continue;

            uint32_t n = socket_bind_select_l3(&s->bindSpec, IP_VER4, ids, MAX_IPV4_L3_INTERFACES);
            for (uint32_t i = 0; i < n; ++i) {
                l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(ids[i]);
                if (!ipv4_l3_is_active(v4)) continue;
                l2_ipv4_mcast_leave(v4->l2->ifindex, g);
            }
        } else if (group->ver == IP_VER6) {
            if (!ipv6_is_multicast(group->ip)) continue;
            bool linkscope = ipv6_is_linkscope_mcast(group->ip);

            uint32_t n = socket_bind_select_l3(&s->bindSpec, IP_VER6, ids, MAX_IPV6_L3_INTERFACES);
            for (uint32_t i = 0; i < n; ++i) {
                l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(ids[i]);
                if (!ipv6_l3_is_ready(v6)) continue;
                if (linkscope && !ipv6_is_linklocal(v6->ip)) continue;
                l2_ipv6_mcast_leave(v6->l2->ifindex, group->ip);
            }
        }
    }
}

static int32_t udp_socket_join_mcast_groups(udp_socket_t* s) {
    if (!s || !s->mcast_groups || !s->mcast_count) return SOCK_OK;
    if (!s->localPort) return SOCK_OK;

    uint8_t ids[MAX_L3_INTERFACES];
    for (uint32_t gidx = 0; gidx < s->mcast_count; ++gidx) {
        const net_l4_endpoint* group = &s->mcast_groups[gidx];
        uint32_t n = 0;
        bool joined = false;

        if (group->ver == IP_VER4) {
            uint32_t g = 0;
            memcpy(&g, group->ip, 4);
            if (!ipv4_is_multicast(g)) return SOCK_ERR_INVAL;
            n = socket_bind_select_l3(&s->bindSpec, IP_VER4, ids, MAX_IPV4_L3_INTERFACES);
            for (uint32_t i = 0; i < n; ++i) {
                l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(ids[i]);
                if (!ipv4_l3_is_active(v4)) continue;
                if (l2_ipv4_mcast_join(v4->l2->ifindex, g)) joined = true;
            }
        } else if (group->ver == IP_VER6) {
            if (!ipv6_is_multicast(group->ip)) return SOCK_ERR_INVAL;
            bool linkscope = ipv6_is_linkscope_mcast(group->ip);
            n = socket_bind_select_l3(&s->bindSpec, IP_VER6, ids, MAX_IPV6_L3_INTERFACES);
            for (uint32_t i = 0; i < n; ++i) {
                l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(ids[i]);
                if (!ipv6_l3_is_ready(v6)) continue;
                if (linkscope && !ipv6_is_linklocal(v6->ip)) continue;
                if (l2_ipv6_mcast_join(v6->l2->ifindex, group->ip)) joined = true;
            }
        } else return SOCK_ERR_INVAL;

        if (!joined) return SOCK_ERR_SYS;
    }

    return SOCK_OK;
}

uint32_t socket_udp_input(ksocket_t* socket, ip_version_t ipver, uint8_t l3_id, const void* src_ip_addr, const void* dst_ip_addr, netpkt_t* pkt, uint16_t src_port, uint16_t dst_port) {
    if (!socket || !pkt) return 0;
    udp_socket_t* s = (udp_socket_t*)socket_core_impl(socket);
    if (!s) return 0;
    if (!s->bindToken || s->localPort != dst_port || !dst_ip_addr || !src_ip_addr) return 0;
    if (ipver != IP_VER4 && ipver != IP_VER6) return 0;

    if (ipver == IP_VER4) {
        uint32_t dip = 0;
        memcpy(&dip, dst_ip_addr, 4);
        if (ipv4_is_multicast(dip) && !udp_socket_mcast_match(s, IP_VER4, dst_ip_addr)) return 0;
    } else if (ipver == IP_VER6) {
        if (ipv6_is_multicast(dst_ip_addr) && !udp_socket_mcast_match(s, IP_VER6, dst_ip_addr)) return 0;
    }

    if (s->connected) {
        if (s->remoteEP.ver != ipver || s->remoteEP.port != src_port) return 0;
        if (ipver == IP_VER4 && memcmp(s->remoteEP.ip, src_ip_addr, 4) != 0) return 0;
        if (ipver == IP_VER6 && ipv6_cmp(s->remoteEP.ip, src_ip_addr) != 0) return 0;
    }

    uint32_t pkt_len = netpkt_len(pkt);
    uint32_t limit = UINT32_MAX;
    if ((s->options.flags & SOCK_OPT_BUF_SIZE) && s->options.buf_size) limit = s->options.buf_size;
    if (pkt_len > limit) return 0;
    if (s->rx_bytes > limit - pkt_len) return 0;

    if (!s->rx_ring || !s->ring_cap) {
        uint32_t usable = UDP_DEFAULT_RING_CAP;
        if ((s->options.flags & SOCK_OPT_BUF_SIZE) && s->options.buf_size) {
            usable = s->options.buf_size / MAX_PACKET_SIZE;
            if (usable < 4) usable = 4;
            if (usable > UDP_MAX_RING_CAP) usable = UDP_MAX_RING_CAP;
        }

        s->ring_cap = usable + 1;
        s->rx_ring = (udp_rx_entry_t*)zalloc(sizeof(udp_rx_entry_t) * s->ring_cap);
        if (!s->rx_ring) {
            s->ring_cap = 0;
            return 0;
        }
    }

    uint32_t nexti = (s->r_tail + 1) % s->ring_cap;
    if (nexti == s->r_head) return 0;

    netpkt_ref(pkt);
    s->rx_ring[s->r_tail].pkt = pkt;
    make_ep(src_ip_addr, src_port, ipver, &s->rx_ring[s->r_tail].src);
    memset(&s->rx_ring[s->r_tail].rx_spec, 0, sizeof(s->rx_ring[s->r_tail].rx_spec));
    s->rx_ring[s->r_tail].rx_spec.kind = BIND_L3;
    s->rx_ring[s->r_tail].rx_spec.ver = ipver;
    s->rx_ring[s->r_tail].rx_spec.l3_id = l3_id;
    s->rx_bytes += pkt_len;
    s->r_tail = nexti;
    return pkt_len;
}

socket_impl_t udp_socket_create(ksocket_t* owner, const SocketOptions* extra) {
    if (!owner) return NULL;

    uint32_t supported = SOCK_OPT_RECV_TIMEOUT | SOCK_OPT_SEND_TIMEOUT | SOCK_OPT_BUF_SIZE | SOCK_OPT_DEBUG | SOCK_OPT_DONTFRAG | SOCK_OPT_BROADCAST_ALLOWED | SOCK_OPT_TTL | SOCK_OPT_MCAST_JOIN;
    if (extra) {
        if (extra->flags & ~supported) return NULL;
        if ((extra->flags & SOCK_OPT_DEBUG) && extra->debug_level > SOCK_DBG_ALL) return NULL;
        if ((extra->flags & SOCK_OPT_BUF_SIZE) && !extra->buf_size) return NULL;
    }

    udp_socket_t* s = (udp_socket_t*)zalloc(sizeof(*s));
    if (!s) return NULL;
    s->ownerSocket = owner;
    s->remoteEP.ver = IP_VER4;
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

    if (s->localPort) return SOCK_ERR_BOUND;
    if (!s->ownerSocket) return SOCK_ERR_SYS;

    SockBindSpec spec = *spec_in;
    if (!socket_bind_normalize_spec(&spec)) return SOCK_ERR_INVAL;

    if (spec.kind == BIND_L3) {
        if (!spec.l3_id) return SOCK_ERR_INVAL;
        uint8_t one[1];
        if (spec.ver == IP_VER4 && !socket_bind_select_l3(&spec, IP_VER4, one, 1)) return SOCK_ERR_INVAL;
        if (spec.ver == IP_VER6 && !socket_bind_select_l3(&spec, IP_VER6, one, 1)) return SOCK_ERR_INVAL;
        if (!spec.ver && !socket_bind_select_l3(&spec, IP_VER4, one, 1) && !socket_bind_select_l3(&spec, IP_VER6, one, 1)) return SOCK_ERR_INVAL;
    } else if (spec.kind == BIND_L2) {
        l2_interface_t* l2 = l2_interface_find_by_index(spec.ifindex);
        if (!l2 || !l2->is_up) return SOCK_ERR_INVAL;
    } else if (spec.kind == BIND_IP) {
        if (spec.ver == IP_VER4) {
            uint32_t v4ip = 0;
            memcpy(&v4ip, spec.ip, 4);
            if (!ipv4_is_unspecified(v4ip)) {
                uint8_t one[1];
                if (!socket_bind_select_l3(&spec, IP_VER4, one, 1)) return SOCK_ERR_INVAL;
            }
        } else if (spec.ver == IP_VER6) {
            if (!ipv6_is_unspecified(spec.ip)) {
                uint8_t one[1];
                if (!socket_bind_select_l3(&spec, IP_VER6, one, 1)) return SOCK_ERR_INVAL;
            }
        } else return SOCK_ERR_INVAL;
    } else if (spec.kind != BIND_ANY && spec.kind != BIND_ANY4 && spec.kind != BIND_ANY6) return SOCK_ERR_INVAL;

    int bind_port = port;
    socket_bind_token_t token = 0;
    if (bind_port == 0) {
        bind_port = socket_bind_alloc_ephemeral(s->ownerSocket, PROTO_UDP, &spec, &token);
        if (bind_port < 0) return SOCK_ERR_NO_PORT;
    } else if (!socket_bind_insert(s->ownerSocket, PROTO_UDP, &spec, port, &token)) return SOCK_ERR_BOUND;

    s->bindSpec = spec;
    s->bindToken = token;
    s->localPort = (uint16_t)bind_port;

    int32_t mcast_res = udp_socket_join_mcast_groups(s);
    if (mcast_res != SOCK_OK) {
        udp_socket_leave_mcast_groups(s);
        socket_bind_remove(s->bindToken);
        s->localPort = 0;
        memset(&s->bindSpec, 0, sizeof(s->bindSpec));
        s->bindToken = 0;
        return mcast_res;
    }

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

    if (!dst || !dst->port) return SOCK_ERR_INVAL;
    if (dst->ver != IP_VER4 && dst->ver != IP_VER6) return SOCK_ERR_INVAL;

    s->remoteEP = *dst;
    s->connected = true;
    return SOCK_OK;
}

int64_t socket_sendto_udp(socket_impl_t sh, const net_l4_endpoint* dst, const void* buf, uint64_t len) {
    udp_socket_t* s = (udp_socket_t*)sh;
    if (!s || (!buf && len)) return SOCK_ERR_INVAL;

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
    if (!buf && len) return SOCK_ERR_INVAL;
    if (!s->ownerSocket) return SOCK_ERR_SYS;

    net_l4_endpoint d = *dst;
    if (s->connected && explicit_dst) {
        if (d.ver != s->remoteEP.ver || d.port != s->remoteEP.port) return SOCK_ERR_STATE;
        if (d.ver == IP_VER4 && memcmp(d.ip, s->remoteEP.ip, 4) != 0) return SOCK_ERR_STATE;
        if (d.ver == IP_VER6 && ipv6_cmp(d.ip, s->remoteEP.ip) != 0) return SOCK_ERR_STATE;
    }

    sizedptr pay;
    pay.ptr = (uintptr_t)buf;
    pay.size = (uint32_t)len;

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
            if (!s->localPort) {
                socket_bind_token_t token = 0;
                int p = socket_bind_alloc_ephemeral_l3(s->ownerSocket, PROTO_UDP, chosen_l3, &token);
                if (p < 0) return SOCK_ERR_NO_PORT;
                s->localPort = (uint16_t)p;
                s->bindToken = token;
                memset(&s->bindSpec, 0, sizeof(s->bindSpec));
                s->bindSpec.kind = BIND_L3;
                s->bindSpec.ver = IP_VER4;
                s->bindSpec.l3_id = chosen_l3;
            }

            net_l4_endpoint src;
            make_ep(&bcast_v4->ip, s->localPort, IP_VER4, &src);

            ip_tx_opts_t tx;
            tx.scope = IP_TX_BOUND_L3;
            tx.index = chosen_l3;

            udp_send_segment(&src, &d, pay, &tx, (s->options.flags & SOCK_OPT_TTL) ? s->options.ttl : 0, (s->options.flags & SOCK_OPT_DONTFRAG) ? 1 : 0);
            return (int64_t)len;
        }

        ipv4_tx_plan_t plan;
        if (!socket_bind_build_ipv4_tx_plan(&s->bindSpec, s->localPort, dip, &plan)) return SOCK_ERR_SYS;

        uint8_t tx_l3 = plan.l3_id;
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(tx_l3);
        if (!ipv4_l3_is_ready(v4)) return SOCK_ERR_SYS;

        if (!s->localPort) {
            socket_bind_token_t token = 0;
            int p = socket_bind_alloc_ephemeral_l3(s->ownerSocket, PROTO_UDP, tx_l3, &token);
            if (p < 0) return SOCK_ERR_NO_PORT;
            s->localPort = (uint16_t)p;
            s->bindToken = token;
            memset(&s->bindSpec, 0, sizeof(s->bindSpec));
            s->bindSpec.kind = BIND_L3;
            s->bindSpec.ver = IP_VER4;
            s->bindSpec.l3_id = tx_l3;
        }

        net_l4_endpoint src;
        make_ep(&v4->ip, s->localPort, IP_VER4, &src);

        ip_tx_opts_t tx;
        tx.scope = IP_TX_BOUND_L3;
        tx.index = plan.l3_id;

        udp_send_segment(&src, &d, pay, &tx, (s->options.flags & SOCK_OPT_TTL) ? s->options.ttl : 0, (s->options.flags & SOCK_OPT_DONTFRAG) ? 1 : 0);
        return (int64_t)len;
    }

    if (d.ver == IP_VER6) {
        ipv6_tx_plan_t plan;
        if (!socket_bind_build_ipv6_tx_plan(&s->bindSpec, s->localPort, d.ip, &plan)) return SOCK_ERR_SYS;

        uint8_t chosen_l3 = plan.l3_id;
        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(chosen_l3);
        if (!ipv6_l3_is_ready(v6)) return SOCK_ERR_SYS;

        if (!s->localPort) {
            socket_bind_token_t token = 0;
            int p = socket_bind_alloc_ephemeral_l3(s->ownerSocket, PROTO_UDP, chosen_l3, &token);
            if (p < 0) return SOCK_ERR_NO_PORT;
            s->localPort = (uint16_t)p;
            s->bindToken = token;
            memset(&s->bindSpec, 0, sizeof(s->bindSpec));
            s->bindSpec.kind = BIND_L3;
            s->bindSpec.ver = IP_VER6;
            s->bindSpec.l3_id = chosen_l3;
        }

        net_l4_endpoint src;
        make_ep(v6->ip, s->localPort, IP_VER6, &src);

        ip_tx_opts_t tx;
        tx.scope = IP_TX_BOUND_L3;
        tx.index = plan.l3_id;

        udp_send_segment(&src, &d, pay, &tx, (s->options.flags & SOCK_OPT_TTL) ? s->options.ttl : 0, (s->options.flags & SOCK_OPT_DONTFRAG) ? 1 : 0);
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

    if (!s->rx_ring || s->r_head == s->r_tail) {
        if (!(s->options.flags & SOCK_OPT_RECV_TIMEOUT) || !s->options.recv_timeout_ms) return SOCK_ERR_WOULDBLOCK;

        uint32_t start_ms = (uint32_t)get_time();
        while (!s->rx_ring || s->r_head == s->r_tail) {
            uint32_t now_ms = (uint32_t)get_time();
            uint32_t elapsed_ms = now_ms - start_ms;
            if (elapsed_ms >= s->options.recv_timeout_ms) return SOCK_ERR_WOULDBLOCK;

            uint32_t wait_ms = s->options.recv_timeout_ms - elapsed_ms;
            if (wait_ms > 5) wait_ms = 5;
            msleep(wait_ms);
        }
    }

    netpkt_t* p = s->rx_ring[s->r_head].pkt;
    net_l4_endpoint se = s->rx_ring[s->r_head].src;
    s->lastRxSpec = s->rx_ring[s->r_head].rx_spec;
    memset(&s->rx_ring[s->r_head], 0, sizeof(s->rx_ring[s->r_head]));
    s->r_head = (s->r_head + 1) % s->ring_cap;

    uint32_t pkt_len = p ? netpkt_len(p) : 0;
    s->rx_bytes -= pkt_len;
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

    switch ((uint32_t)opt) {
        case SOCK_OPT_KEEPALIVE:
        case SOCK_OPT_KEEPALIVE_INTERVAL:
        case SOCK_OPT_TCP_NO_DELAY:
        case SOCK_OPT_SEND_BUF_SIZE:
        case SOCK_OPT_FILTER:
        case SOCK_OPT_SPECIAL:
            return SOCK_ERR_UNSUP;
        case SOCK_OPT_MCAST_JOIN: {
            if (!value || !len || (len % sizeof(net_l4_endpoint)) != 0) return SOCK_ERR_INVAL;
            uint32_t count32 = len / sizeof(net_l4_endpoint);
            if (!count32 || count32 > 255 || count32 + s->mcast_count > 255) return SOCK_ERR_INVAL;

            const net_l4_endpoint* groups = value;
            for (uint32_t i = 0; i < count32; ++i) if (!udp_socket_mcast_endpoint_valid(&groups[i])) return SOCK_ERR_INVAL;

            net_l4_endpoint* next = (net_l4_endpoint*)zalloc(sizeof(net_l4_endpoint) * (s->mcast_count + count32));
            if (!next) return SOCK_ERR_SYS;
            if (s->mcast_count) memcpy(next, s->mcast_groups, sizeof(net_l4_endpoint) * s->mcast_count);

            uint8_t next_count = s->mcast_count;
            for (uint32_t i = 0; i < count32; ++i) {
                bool exists = false;
                for (uint8_t j = 0; j < next_count; ++j) {
                    if (next[j].ver != groups[i].ver) continue;
                    uint32_t sz = groups[i].ver == IP_VER4 ? 4 : 16;
                    if (memcmp(next[j].ip, groups[i].ip, sz) == 0) exists = true;
                }
                if (!exists) {
                    next[next_count] = groups[i];
                    next[next_count].port = 0;
                    next_count++;
                }
            }

            if (next_count == s->mcast_count) {
                release(next);
                return SOCK_OK;
            }

            net_l4_endpoint* old_groups = s->mcast_groups;
            uint8_t old_count = s->mcast_count;
            if (s->localPort) udp_socket_leave_mcast_groups(s);

            s->mcast_groups = next;
            s->mcast_count = next_count;
            int32_t rc = udp_socket_join_mcast_groups(s);
            if (rc != SOCK_OK) {
                udp_socket_leave_mcast_groups(s);
                release(next);
                s->mcast_groups = old_groups;
                s->mcast_count = old_count;
                s->options.mcast_groups = s->mcast_groups;
                s->options.mcast_count = s->mcast_count;
                if (s->mcast_count) s->options.flags |= SOCK_OPT_MCAST_JOIN;
                else s->options.flags &= ~SOCK_OPT_MCAST_JOIN;
                if (s->localPort) udp_socket_join_mcast_groups(s);
                return rc;
            }

            if (old_groups) release(old_groups);
            s->options.mcast_groups = s->mcast_groups;
            s->options.mcast_count = s->mcast_count;
            s->options.flags |= SOCK_OPT_MCAST_JOIN;
            return SOCK_OK;
        }
        case SOCK_OPT_MCAST_LEAVE: {
            if (!value || !len || (len % sizeof(net_l4_endpoint)) != 0) return SOCK_ERR_INVAL;
            uint32_t count = len / sizeof(net_l4_endpoint);
            if (!count || count > 255) return SOCK_ERR_INVAL;
            if (!s->mcast_groups || !s->mcast_count) return SOCK_OK;

            const net_l4_endpoint* groups = value;
            for (uint32_t i = 0; i < count; ++i) if (!udp_socket_mcast_endpoint_valid(&groups[i])) return SOCK_ERR_INVAL;

            net_l4_endpoint* next = NULL;
            if (s->mcast_count > count) {
                next = (net_l4_endpoint*)zalloc(sizeof(net_l4_endpoint) * s->mcast_count);
                if (!next) return SOCK_ERR_SYS;
            }

            uint8_t next_count = 0;
            for (uint8_t i = 0; i < s->mcast_count; ++i) {
                bool remove_group = false;
                for (uint32_t j = 0; j < count; ++j) {
                    if (s->mcast_groups[i].ver != groups[j].ver) continue;
                    uint32_t sz = groups[j].ver == IP_VER4 ? 4 : 16;
                    if (memcmp(s->mcast_groups[i].ip, groups[j].ip, sz) == 0) remove_group = true;
                }
                if (!remove_group) {
                    if (!next) {
                        next = (net_l4_endpoint*)zalloc(sizeof(net_l4_endpoint) * s->mcast_count);
                        if (!next) return SOCK_ERR_SYS;
                    }
                    next[next_count++] = s->mcast_groups[i];
                }
            }

            if (next_count == s->mcast_count) {
                if (next) release(next);
                return SOCK_OK;
            }

            if (!next_count && next) {
                release(next);
                next = NULL;
            }

            net_l4_endpoint* old_groups = s->mcast_groups;
            uint8_t old_count = s->mcast_count;
            if (s->localPort) udp_socket_leave_mcast_groups(s);

            s->mcast_groups = next;
            s->mcast_count = next_count;
            int32_t rc = udp_socket_join_mcast_groups(s);
            if (rc != SOCK_OK) {
                udp_socket_leave_mcast_groups(s);
                if (next) release(next);
                s->mcast_groups = old_groups;
                s->mcast_count = old_count;
                s->options.mcast_groups = s->mcast_groups;
                s->options.mcast_count = s->mcast_count;
                s->options.flags |= SOCK_OPT_MCAST_JOIN;
                if (s->localPort) udp_socket_join_mcast_groups(s);
                return rc;
            }

            release(old_groups);
            s->options.mcast_count = s->mcast_count;
            s->options.mcast_groups = s->mcast_groups;
            if (s->mcast_count) s->options.flags |= SOCK_OPT_MCAST_JOIN;
            else s->options.flags &= ~SOCK_OPT_MCAST_JOIN;
            return SOCK_OK;
        }
        case SOCK_OPT_RECV_TIMEOUT:
        case SOCK_OPT_SEND_TIMEOUT:
        case SOCK_OPT_BUF_SIZE:
        case SOCK_OPT_DEBUG:
        case SOCK_OPT_DONTFRAG:
        case SOCK_OPT_BROADCAST_ALLOWED:
        case SOCK_OPT_TTL:
            return socket_common_options_set(&s->options, opt, value, len);
        default:
            return SOCK_ERR_INVAL;
    }
}

int32_t socket_getopt_udp(socket_impl_t sh, int32_t opt, void* value, uint32_t* len) {
    udp_socket_t* s = (udp_socket_t*)sh;
    if (!s || !len) return SOCK_ERR_INVAL;

    switch ((uint32_t)opt) {
        case SOCK_GET_REMOTE_ENDPOINT:
            return socket_common_get_value(&s->remoteEP, sizeof(s->remoteEP), value, len);
        case SOCK_GET_BIND_SPEC:
            return socket_common_get_value(&s->bindSpec, sizeof(s->bindSpec), value, len);
        case SOCK_GET_LAST_RX_SPEC:
            return socket_common_get_value(&s->lastRxSpec, sizeof(s->lastRxSpec), value, len);
        default:
            break;
    }

    uint32_t v = 0;
    switch ((uint32_t)opt) {
        case SOCK_GET_BOUND:
            v = s->localPort != 0;
            break;
        case SOCK_GET_CONNECTED:
            v = s->connected;
            break;
        case SOCK_GET_LISTENING:
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
        case SOCK_GET_OPT_FILTER:
        case SOCK_GET_TCP_STATE:
        case SOCK_GET_TCP_MSS:
        case SOCK_GET_TCP_RTT_MS:
        case SOCK_GET_TCP_RETRANSMITS:
            return SOCK_ERR_UNSUP;
        case SOCK_GET_MCAST_GROUPS: {
            uint32_t need = s->mcast_count * sizeof(net_l4_endpoint);
            return socket_common_get_value(s->mcast_groups, need, value, len);
        }
        case SOCK_GET_OPT_RECV_TIMEOUT:
        case SOCK_GET_OPT_SEND_TIMEOUT:
        case SOCK_GET_OPT_BUF_SIZE:
        case SOCK_GET_OPT_DEBUG:
        case SOCK_GET_OPT_DONTFRAG:
        case SOCK_GET_OPT_BROADCAST_ALLOWED:
        case SOCK_GET_OPT_TTL:
            return socket_common_options_get(&s->options, opt, value, len);
        default:
            return SOCK_ERR_INVAL;
    }

    return socket_common_get_value(&v, sizeof(v), value, len);
}

int32_t socket_close_udp(socket_impl_t sh) {
    udp_socket_t* s = (udp_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;

    udp_socket_leave_mcast_groups(s);

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_UDP;
    ev.action = NETLOG_ACT_CLOSE;
    ev.pid = socket_core_pid(s->ownerSocket);
    ev.local_port = s->localPort;
    ev.remote_ep = s->remoteEP;
    netlog_socket_event(&s->options, &ev);

    if (s->rx_ring) {
        while (s->r_head != s->r_tail) {
            if (s->rx_ring[s->r_head].pkt) {
                s->rx_bytes -= netpkt_len(s->rx_ring[s->r_head].pkt);
                netpkt_unref(s->rx_ring[s->r_head].pkt);
            }
            s->r_head = (s->r_head + 1) % s->ring_cap;
        }
        release(s->rx_ring);
        s->rx_ring = NULL;
    }
    if (s->mcast_groups) {
        release(s->mcast_groups);
        s->mcast_groups = NULL;
    }
    s->mcast_count = 0;
    s->options.mcast_count = 0;
    s->options.mcast_groups = NULL;
    s->options.flags &= ~(SOCK_OPT_MCAST_JOIN | SOCK_OPT_MCAST_LEAVE);
    s->ring_cap = 0;
    s->r_head = 0;
    s->r_tail = 0;
    s->rx_bytes = 0;
    memset(&s->lastRxSpec, 0, sizeof(s->lastRxSpec));
    s->lastRxSpec.kind = BIND_ANY;
    if (s->bindToken) {
        socket_bind_remove(s->bindToken);
        s->bindToken = 0;
    }
    s->localPort = 0;
    memset(&s->bindSpec, 0, sizeof(s->bindSpec));
    s->connected = false;
    s->remoteEP.port = 0;
    s->remoteEP.ver = IP_VER4;
    memset(s->remoteEP.ip, 0, sizeof(s->remoteEP.ip));
    return SOCK_OK;
}

void socket_destroy_udp(socket_impl_t sh) {
    udp_socket_t* s = (udp_socket_t*)sh;
    if (!s) return;
    socket_close_udp(s);
    release(s);
}
