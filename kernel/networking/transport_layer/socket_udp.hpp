#pragma once
#include "socket.hpp"
#include "networking/transport_layer/udp.h"
#include "types.h"
#include "std/memory.h"
#include "net/socket_types.h"
#include "networking/internet_layer/ipv4_route.h"
#include "networking/internet_layer/ipv6_route.h"
#include "syscalls/syscalls.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/internet_layer/ipv6.h"
#include "exceptions/irq.h"
#include "sysregs.h"

static constexpr int32_t UDP_RING_CAP = 1024;

class UDPSocket : public Socket {
    sizedptr ring[UDP_RING_CAP];
    net_l4_endpoint src_eps[UDP_RING_CAP];
    int32_t r_head = 0;
    int32_t r_tail = 0;
    uint32_t rx_bytes = 0;


    static bool is_valid_v4_l3_for_bind(l3_ipv4_interface_t* v4) {
        if (!v4) return false;
        if (!v4->l2) return false;
        if (!v4->l2->is_up) return false;
        if (v4->mode == IPV4_CFG_DISABLED) return false;
        return true;
    }

    static bool is_valid_v6_l3_for_bind(l3_ipv6_interface_t* v6) {
        if (!v6) return false;
        if (!v6->l2) return false;
        if (!v6->l2->is_up) return false;
        if (v6->cfg == IPV6_CFG_DISABLE) return false;
        if (v6->dad_state != IPV6_DAD_OK) return false;
        return true;
    }

    static bool udp_mcast_match(UDPSocket* s, ip_version_t ver, const void* dst_ip_addr) {
        if (!s || !dst_ip_addr) return false;
        if ((s->extraOpts.flags & SOCK_OPT_MCAST_JOIN) == 0) return false;

        uint32_t count = s->extraOpts.mcast_count;
        if (count > SOCK_MAX_MCAST_GROUPS) count = SOCK_MAX_MCAST_GROUPS;

        for (uint32_t i = 0; i < count; ++i) {
            const net_l4_endpoint* group = &s->extraOpts.mcast_groups[i];
            if (group->ver != ver) continue;

            if (ver == IP_VER4) {
                uint32_t want = 0;
                uint32_t got = 0;
                memcpy(&want, group->ip, 4);
                memcpy(&got, dst_ip_addr, 4);
                if (want == got) return true;
            } else if (ver == IP_VER6) {
                if (memcmp(group->ip, dst_ip_addr, 16) == 0) return true;
            }
        }

        return false;
    }

    static bool is_dbcast(uint32_t ip, uint8_t* out_l3) {
        uint8_t cnt = l2_interface_count();
        for (uint8_t i = 0; i < cnt; ++i) {
            l2_interface_t* l2 = l2_interface_at(i);
            if (!l2) continue;
            for (int s = 0; s < MAX_IPV4_PER_INTERFACE; ++s) {
                l3_ipv4_interface_t* v4 = l2->l3_v4[s];
                if (!v4) continue;
                if (v4->mode == IPV4_CFG_DISABLED) continue;
                if (!v4->ip) continue;
                if (!v4->mask) continue;
                uint32_t b = ipv4_broadcast_calc(v4->ip, v4->mask);
                if (b != ip) continue;
                if (out_l3) *out_l3 = v4->l3_id;
                return true;
            }
        }
        return false;
    }

    void on_receive(ip_version_t ver, const void* src_ip_addr, uint16_t src_port, netpkt_t* pkt) {
        if (!pkt) return;
        uintptr_t ptr = netpkt_data(pkt);
        uint32_t len = netpkt_len(pkt);
        uint32_t limit = 0xFFFFFFFFu;
        if ((extraOpts.flags & SOCK_OPT_BUF_SIZE) && extraOpts.buf_size) limit = extraOpts.buf_size;
        if (len > limit) return;
        if (rx_bytes > limit - len) return;

        int nexti = (r_tail + 1) % UDP_RING_CAP;
        if (nexti == r_head) return;

        uintptr_t copy = 0;
        if (len) {
            copy = (uintptr_t)zalloc(len);
            if (!copy) return;
            memcpy((void*)copy, (const void*)ptr, len);
        }

        ring[r_tail].ptr = copy;
        ring[r_tail].size = len;
        rx_bytes += len;

        src_eps[r_tail].ver = ver;
        memset(src_eps[r_tail].ip, 0, 16);

        if (ver == IP_VER4) {
            memcpy(src_eps[r_tail].ip, src_ip_addr, 4);
        } else if (ver == IP_VER6) {
            memcpy(src_eps[r_tail].ip, src_ip_addr, 16);
        }

        src_eps[r_tail].port = src_port;

        r_tail = nexti;
        remoteEP = src_eps[(r_tail + UDP_RING_CAP - 1) % UDP_RING_CAP];
    }

    void leave_mcast_groups() {
        if ((extraOpts.flags & SOCK_OPT_MCAST_JOIN) == 0) return;

        uint32_t count = extraOpts.mcast_count;
        if (count > SOCK_MAX_MCAST_GROUPS) count = SOCK_MAX_MCAST_GROUPS;

        uint8_t ids[MAX_L2_INTERFACES * MAX_IPV6_PER_INTERFACE];
        for (uint32_t gidx = 0; gidx < count; ++gidx) {
            const net_l4_endpoint* group = &extraOpts.mcast_groups[gidx];

            if (group->ver == IP_VER4) {
                uint32_t g = 0;
                memcpy(&g, group->ip, 4);
                if (!ipv4_is_multicast(g)) continue;

                uint32_t n = socket_bind_l3_list(&bindSpec, IP_VER4, ids, MAX_L2_INTERFACES * MAX_IPV4_PER_INTERFACE);
                for (uint32_t i = 0; i < n; ++i) {
                    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(ids[i]);
                    if (!is_valid_v4_l3_for_bind(v4)) continue;
                    l2_ipv4_mcast_leave(v4->l2->ifindex,g);
                }
            } else if (group->ver == IP_VER6) {
                if (!ipv6_is_multicast(group->ip)) continue;
                bool linkscope = ipv6_is_linkscope_mcast(group->ip);

                uint32_t n = socket_bind_l3_list(&bindSpec, IP_VER6, ids, MAX_L2_INTERFACES * MAX_IPV6_PER_INTERFACE);
                for (uint32_t i = 0; i < n; ++i) {
                    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(ids[i]);
                    if (!is_valid_v6_l3_for_bind(v6)) continue;
                    if (linkscope && !ipv6_is_linklocal(v6->ip)) continue;
                    l2_ipv6_mcast_leave(v6->l2->ifindex,group->ip);
                }
            }
        }
    }

public:
    UDPSocket(ksocket_t* owner, uint8_t r, uint32_t pid_, const SocketExtraOptions* extra = nullptr) : Socket(owner, PROTO_UDP, r, extra) {
        pid = pid_;
    }

    ~UDPSocket() override {
        close();
    }

    int32_t bind(const SockBindSpec& spec_in, uint16_t port) override {
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_UDP;
        ev.action = NETLOG_ACT_BIND;
        ev.pid = pid;
        ev.u0 = port;
        ev.bind_spec = spec_in;
        netlog_socket_event(&extraOpts, &ev);
        if (role != SOCKET_SERVER) return SOCK_ERR_PERM;
        if (bound) return SOCK_ERR_BOUND;
        if (!ownerSocket) return SOCK_ERR_SYS;

        SockBindSpec spec = spec_in;
        bool empty = spec.kind == BIND_L3 && spec.l3_id == 0 && spec.ifindex == 0 && spec.ver == 0 && ipv6_is_unspecified(spec.ip);
        if (empty) spec.kind = BIND_ANY;

        if (spec.kind == BIND_L3) {
            if (!spec.l3_id) return SOCK_ERR_INVAL;
            uint8_t one[1];
            if (spec.ver == IP_VER4 && !socket_bind_l3_list(&spec, IP_VER4, one, 1)) return SOCK_ERR_INVAL;
            if (spec.ver == IP_VER6 && !socket_bind_l3_list(&spec, IP_VER6, one, 1)) return SOCK_ERR_INVAL;
            if (!spec.ver && !socket_bind_l3_list(&spec, IP_VER4, one, 1) && !socket_bind_l3_list(&spec, IP_VER6, one, 1)) return SOCK_ERR_INVAL;
        } else if (spec.kind == BIND_L2) {
            l2_interface_t* l2 = l2_interface_find_by_index(spec.ifindex);
            if (!l2 || !l2->is_up) return SOCK_ERR_INVAL;
        } else if (spec.kind == BIND_IP) {
            if (spec.ver == IP_VER4) {
                uint32_t v4ip = 0;
                memcpy(&v4ip, spec.ip, 4);
                if (!ipv4_is_unspecified(v4ip)) {
                    uint8_t one[1];
                    if (!socket_bind_l3_list(&spec, IP_VER4, one, 1)) return SOCK_ERR_INVAL;
                }
            } else if (spec.ver == IP_VER6) {
                if (!ipv6_is_unspecified(spec.ip)) {
                    uint8_t one[1];
                    if (!socket_bind_l3_list(&spec, IP_VER6, one, 1)) return SOCK_ERR_INVAL;
                }
            } else return SOCK_ERR_INVAL;
        } else if (spec.kind != BIND_ANY && spec.kind != BIND_ANY4 && spec.kind != BIND_ANY6) return SOCK_ERR_INVAL;

        if (!socket_bind_insert(ownerSocket, PROTO_UDP, &spec, port)) return SOCK_ERR_SYS;

        bindSpec = spec;
        localPort = port;
        bound = true;

        if (extraOpts.flags & SOCK_OPT_MCAST_JOIN) {
            uint32_t count = extraOpts.mcast_count;
            if (count > SOCK_MAX_MCAST_GROUPS) count = SOCK_MAX_MCAST_GROUPS;
            uint8_t ids[MAX_L2_INTERFACES * MAX_IPV6_PER_INTERFACE];

            for (uint32_t gidx = 0; gidx < count; ++gidx) {
                const net_l4_endpoint* group = &extraOpts.mcast_groups[gidx];
                uint32_t n = 0;
                bool joined = false;

                if (group->ver == IP_VER4) {
                    uint32_t g = 0;
                    memcpy(&g, group->ip, 4);
                    if (!ipv4_is_multicast(g)) continue;
                    n = socket_bind_l3_list(&bindSpec, IP_VER4, ids, MAX_L2_INTERFACES * MAX_IPV4_PER_INTERFACE);
                    for (uint32_t i = 0; i < n; ++i) {
                        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(ids[i]);
                        if (!is_valid_v4_l3_for_bind(v4)) continue;
                        if (l2_ipv4_mcast_join(v4->l2->ifindex, g)) joined = true;
                    }
                } else if (group->ver == IP_VER6) {
                    if (!ipv6_is_multicast(group->ip)) continue;
                    bool linkscope = ipv6_is_linkscope_mcast(group->ip);
                    n = socket_bind_l3_list(&bindSpec, IP_VER6, ids, MAX_L2_INTERFACES * MAX_IPV6_PER_INTERFACE);
                    for (uint32_t i = 0; i < n; ++i) {
                        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(ids[i]);
                        if (!is_valid_v6_l3_for_bind(v6)) continue;
                        if (linkscope && !ipv6_is_linklocal(v6->ip)) continue;
                        if (l2_ipv6_mcast_join(v6->l2->ifindex, group->ip)) joined = true;
                    }
                }

                if (!joined) {
                    leave_mcast_groups();
                    socket_bind_remove_socket(ownerSocket);
                    memset(&bindSpec, 0, sizeof(bindSpec));
                    bound = false;
                    localPort = 0;
                    return SOCK_ERR_SYS;
                }
            }
        }

        return SOCK_OK;
    }

    int64_t sendto(const net_l4_endpoint* dst, const void* buf, uint64_t len) {
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_UDP;
        ev.action = NETLOG_ACT_SENDTO;
        ev.pid = pid;
        ev.u1 = (uint32_t)len;
        if (dst) {
            ev.dst_ep = *dst;
            ev.u0 = dst->port;
        }
        netlog_socket_event(&extraOpts, &ev);
        if (!dst || !dst->port) return SOCK_ERR_INVAL;
        if (!buf || !len) return SOCK_ERR_INVAL;
        if (!bound && !ownerSocket) return SOCK_ERR_SYS;

        net_l4_endpoint d = *dst;
        sizedptr pay;
        pay.ptr = (uintptr_t)buf;
        pay.size = (uint32_t)len;
        
        if (d.ver == IP_VER4) {
            uint32_t dip = 0;
            memcpy(&dip, d.ip, 4);

            if (dip == 0xFFFFFFFFu || is_dbcast(dip, nullptr)) {
                if (!bound || !localPort) return SOCK_ERR_BOUND;

                uint8_t ids[MAX_L2_INTERFACES * MAX_IPV4_PER_INTERFACE];
                uint32_t n = socket_bind_l3_list(&bindSpec, IP_VER4, ids, MAX_L2_INTERFACES * MAX_IPV4_PER_INTERFACE);
                if (!n) return SOCK_ERR_SYS;

                if (dip == 0xFFFFFFFFu) {
                    for (uint32_t i = 0; i < n; ++i) {
                        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(ids[i]);
                        if (!is_valid_v4_l3_for_bind(v4) || !v4->l2) continue;

                        net_l4_endpoint src;
                        src.ver = IP_VER4;
                        memset(src.ip, 0, 16);
                        memcpy(src.ip, &v4->ip, 4);
                        src.port = localPort;

                        ipv4_tx_opts_t tx;
                        tx.scope = IP_TX_BOUND_L3;
                        tx.index = ids[i];

                        udp_send_segment(&src, &d, pay, &tx, (extraOpts.flags & SOCK_OPT_TTL) ? extraOpts.ttl : 0, (extraOpts.flags & SOCK_OPT_DONTFRAG) ? 1 : 0);
                    }

                    remoteEP = d;
                    return (int64_t)len;
                }

                uint8_t db_l3 = 0;
                if (!is_dbcast(dip, &db_l3)) return SOCK_ERR_SYS;
                bool allowed = false;
                for (uint32_t i = 0; i < n; ++i) if (ids[i] == db_l3) allowed = true;
                if (!allowed) return SOCK_ERR_SYS;

                l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(db_l3);
                if (!is_valid_v4_l3_for_bind(v4) || !v4->l2) return SOCK_ERR_SYS;

                net_l4_endpoint src;
                src.ver = IP_VER4;
                memset(src.ip, 0, 16);
                memcpy(src.ip, &v4->ip, 4);
                src.port = localPort;

                ipv4_tx_opts_t tx;
                tx.scope = IP_TX_BOUND_L3;
                tx.index = db_l3;

                udp_send_segment(&src, &d, pay, &tx, (extraOpts.flags & SOCK_OPT_TTL) ? extraOpts.ttl : 0, (extraOpts.flags & SOCK_OPT_DONTFRAG) ? 1 : 0);
                remoteEP = d;
                return (int64_t)len;
            }

            if (ipv4_is_multicast(dip)) {
                if (!bound || !localPort) return SOCK_ERR_BOUND;

                uint8_t ids[MAX_L2_INTERFACES * MAX_IPV4_PER_INTERFACE];
                uint32_t n = socket_bind_l3_list(&bindSpec, IP_VER4, ids, MAX_L2_INTERFACES * MAX_IPV4_PER_INTERFACE);
                if (!n) return SOCK_ERR_SYS;

                for (uint32_t i = 0; i < n; ++i) {
                    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(ids[i]);
                    if (!is_valid_v4_l3_for_bind(v4) || !v4->l2) continue;

                    net_l4_endpoint src;
                    src.ver = IP_VER4;
                    memset(src.ip, 0, 16);
                    memcpy(src.ip, &v4->ip, 4);
                    src.port = localPort;

                    ipv4_tx_opts_t tx;
                    tx.scope = IP_TX_BOUND_L3;
                    tx.index = ids[i];

                    udp_send_segment(&src, &d, pay, &tx, (extraOpts.flags & SOCK_OPT_TTL) ? extraOpts.ttl : 0, (extraOpts.flags & SOCK_OPT_DONTFRAG) ? 1 : 0);
                }

                remoteEP = d;
                return (int64_t)len;
            }

            uint8_t allowed_v4[MAX_L2_INTERFACES * MAX_IPV4_PER_INTERFACE];
            uint32_t n_allowed = bound ? socket_bind_l3_list(&bindSpec, IP_VER4, allowed_v4, MAX_L2_INTERFACES * MAX_IPV4_PER_INTERFACE) : 0;
            if (bound && n_allowed == 0) return SOCK_ERR_SYS;

            ipv4_tx_plan_t plan;
            if (!ipv4_build_tx_plan(dip, nullptr, n_allowed ? allowed_v4 : nullptr, n_allowed, &plan)) return SOCK_ERR_SYS;

            uint8_t chosen_l3 = plan.l3_id;
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
            if (!is_valid_v4_l3_for_bind(v4)) return SOCK_ERR_SYS;

            if (!bound) {
                int p = socket_bind_alloc_ephemeral_l3(ownerSocket, PROTO_UDP, chosen_l3, pid);
                if (p < 0) return SOCK_ERR_NO_PORT;
                localPort = (uint16_t)p;
                bindSpec = {};
                bindSpec.kind = BIND_L3;
                bindSpec.ver = IP_VER4;
                bindSpec.l3_id = chosen_l3;
                bound = true;
            } else if (localPort == 0) {
                int p = socket_bind_alloc_ephemeral_l3(ownerSocket, PROTO_UDP, chosen_l3, pid);
                if (p < 0) return SOCK_ERR_NO_PORT;
                localPort = (uint16_t)p;
            }

            net_l4_endpoint src;
            src.ver = IP_VER4;
            memset(src.ip, 0, 16);
            memcpy(src.ip, &v4->ip, 4);
            src.port = localPort;

            ipv4_tx_opts_t tx;
            tx.scope = (ip_tx_scope_t)plan.fixed_opts.scope;
            tx.index = plan.fixed_opts.index;

            udp_send_segment(&src, &d, pay, &tx, (extraOpts.flags & SOCK_OPT_TTL) ? extraOpts.ttl : 0, (extraOpts.flags & SOCK_OPT_DONTFRAG) ? 1 : 0);
            remoteEP = d;
            return (int64_t)len;
        }

        if (d.ver == IP_VER6) {
            bool is_mcast = ipv6_is_multicast(d.ip);

            if (is_mcast) {
                if (!bound || !localPort) return SOCK_ERR_BOUND;

                uint8_t ids[MAX_L2_INTERFACES * MAX_IPV6_PER_INTERFACE];
                uint32_t n = socket_bind_l3_list(&bindSpec, IP_VER6, ids, MAX_L2_INTERFACES * MAX_IPV6_PER_INTERFACE);
                if (!n) return SOCK_ERR_SYS;

                int sent = 0;
                bool linkscope = ipv6_is_linkscope_mcast(d.ip);
                for (uint32_t i = 0; i < n; ++i) {
                    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(ids[i]);
                    if (!is_valid_v6_l3_for_bind(v6)) continue;
                    if (linkscope && !ipv6_is_linklocal(v6->ip)) continue;

                    net_l4_endpoint src;
                    src.ver = IP_VER6;
                    memset(src.ip, 0, 16);
                    memcpy(src.ip, v6->ip, 16);
                    src.port = localPort;

                    ipv6_tx_opts_t tx;
                    tx.scope = IP_TX_BOUND_L3;
                    tx.index = ids[i];

                    udp_send_segment(&src, &d, pay, &tx, (extraOpts.flags & SOCK_OPT_TTL) ? extraOpts.ttl : 0, (extraOpts.flags & SOCK_OPT_DONTFRAG) ? 1 : 0);
                    sent++;
                }

                if (!sent) return SOCK_ERR_SYS;
                remoteEP = d;
                return (int64_t)len;
            }

            uint8_t allowed_v6[MAX_L2_INTERFACES * MAX_IPV6_PER_INTERFACE];
            uint32_t n_allowed = bound ? socket_bind_l3_list(&bindSpec, IP_VER6, allowed_v6, MAX_L2_INTERFACES * MAX_IPV6_PER_INTERFACE) : 0;
            if (bound && n_allowed == 0) return SOCK_ERR_SYS;

            ipv6_tx_plan_t plan;
            if (!ipv6_build_tx_plan(d.ip, nullptr, n_allowed ? allowed_v6 : nullptr, n_allowed, &plan)) return SOCK_ERR_SYS;

            uint8_t chosen_l3 = plan.l3_id;

            l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(chosen_l3);
            if (!is_valid_v6_l3_for_bind(v6)) return SOCK_ERR_SYS;

            if (!bound) {
                int p = socket_bind_alloc_ephemeral_l3(ownerSocket, PROTO_UDP, chosen_l3, pid);
                if (p < 0) return SOCK_ERR_NO_PORT;
                localPort = (uint16_t)p;
                bindSpec = {};
                bindSpec.kind = BIND_L3;
                bindSpec.ver = IP_VER6;
                bindSpec.l3_id = chosen_l3;
                bound = true;
            } else if (localPort == 0) {
                int p = socket_bind_alloc_ephemeral_l3(ownerSocket, PROTO_UDP, chosen_l3, pid);
                if (p < 0) return SOCK_ERR_NO_PORT;
                localPort = (uint16_t)p;
            }

            net_l4_endpoint src;
            src.ver = IP_VER6;
            memset(src.ip, 0, 16);
            memcpy(src.ip, v6->ip, 16);
            src.port = localPort;

            ipv6_tx_opts_t tx;
            tx.scope = (ip_tx_scope_t)plan.fixed_opts.scope;
            tx.index = plan.fixed_opts.index;

            udp_send_segment(&src, &d, pay, &tx, (extraOpts.flags & SOCK_OPT_TTL) ? extraOpts.ttl : 0, (extraOpts.flags & SOCK_OPT_DONTFRAG) ? 1 : 0);
            remoteEP = d;
            return (int64_t)len;
        }

        return SOCK_ERR_INVAL;
    }

    uint32_t enqueue_datagram(uint8_t ifindex, uint8_t l3_id, ip_version_t ipver, const void* src_ip_addr, const void* dst_ip_addr, netpkt_t* pkt, uint16_t src_port, uint16_t dst_port) {
        if (!bound || localPort != dst_port || !pkt || !dst_ip_addr) return 0;
        if (ipver == IP_VER4) {
            uint32_t dip = 0;
            memcpy(&dip, dst_ip_addr, 4);
            if (ipv4_is_multicast(dip) && !udp_mcast_match(this, IP_VER4, dst_ip_addr)) return 0;
        } else if (ipver == IP_VER6) {
            if (ipv6_is_multicast((const uint8_t*)dst_ip_addr) && !udp_mcast_match(this, IP_VER6, dst_ip_addr)) return 0;
        }
        on_receive(ipver, src_ip_addr, src_port, pkt);
        return netpkt_len(pkt);
    }

    int64_t recvfrom(void* buf, uint64_t len, net_l4_endpoint* src) {
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_UDP;
        ev.action = NETLOG_ACT_RECVFROM;
        ev.pid = pid;
        ev.u0 = (uint32_t)len;
        ev.local_port = localPort;
        ev.remote_ep = remoteEP;
        netlog_socket_event(&extraOpts, &ev);
        if (r_head == r_tail) return 0;

        sizedptr p = ring[r_head];
        net_l4_endpoint se = src_eps[r_head];
        r_head = (r_head + 1) % UDP_RING_CAP;
        rx_bytes -= p.size;

        uint32_t tocpy = p.size;
        if (tocpy > len) tocpy = (uint32_t)len;

        memcpy(buf, (void*)p.ptr, tocpy);
        if (src) *src = se;

        release((void*)p.ptr);
        remoteEP = se;
        return tocpy;
    }


    int32_t set_option(int32_t opt, const void* value, uint32_t len) {
        switch ((uint32_t)opt) {
            case SOCK_OPT_KEEPALIVE:
            case SOCK_OPT_KEEPALIVE_INTERVAL:
            case SOCK_OPT_TCP_NO_DELAY:
            case SOCK_OPT_SEND_BUF_SIZE:
                return SOCK_ERR_INVAL;
            case SOCK_OPT_MCAST_JOIN: {
                if (!value || !len || (len % sizeof(net_l4_endpoint)) != 0) return SOCK_ERR_INVAL;
                if (bound) return SOCK_ERR_STATE;

                uint32_t count = len / sizeof(net_l4_endpoint);
                if (!count || count > SOCK_MAX_MCAST_GROUPS) return SOCK_ERR_INVAL;

                const net_l4_endpoint* groups = (const net_l4_endpoint*)value;
                for (uint32_t i = 0; i < count; ++i) {
                    if (groups[i].ver != IP_VER4 && groups[i].ver != IP_VER6) return SOCK_ERR_INVAL;
                    if (groups[i].ver == IP_VER4) {
                        uint32_t ip = 0;
                        memcpy(&ip, groups[i].ip, 4);
                        if (!ipv4_is_multicast(ip)) return SOCK_ERR_INVAL;
                    } else if (!ipv6_is_multicast(groups[i].ip)) return SOCK_ERR_INVAL;
                }

                extraOpts.flags |= SOCK_OPT_MCAST_JOIN;
                extraOpts.mcast_count = (uint8_t)count;
                memcpy(extraOpts.mcast_groups, groups, count * sizeof(net_l4_endpoint));
                return SOCK_OK;
            }
            default:
                return Socket::set_option(opt, value, len);
        }
    }

    int32_t get_option(int32_t opt, void* value, uint32_t* len) const {
        switch ((uint32_t)opt) {
            case SOCK_OPT_KEEPALIVE:
            case SOCK_OPT_KEEPALIVE_INTERVAL:
            case SOCK_OPT_TCP_NO_DELAY:
            case SOCK_OPT_SEND_BUF_SIZE:
                return SOCK_ERR_INVAL;
            case SOCK_OPT_MCAST_JOIN: {
                if (!value || !len) return SOCK_ERR_INVAL;
                uint32_t count = (extraOpts.flags & SOCK_OPT_MCAST_JOIN) ? extraOpts.mcast_count : 0;
                if (count > SOCK_MAX_MCAST_GROUPS) count = SOCK_MAX_MCAST_GROUPS;
                uint32_t need = count * sizeof(net_l4_endpoint);
                if (*len < need) return SOCK_ERR_INVAL;
                if (need) memcpy(value, extraOpts.mcast_groups, need);
                *len = need;
                return SOCK_OK;
            }
            default:
                return Socket::get_option(opt, value, len);
        }
    }

    int32_t close() override {
        leave_mcast_groups();
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_UDP;
        ev.action = NETLOG_ACT_CLOSE;
        ev.pid = pid;
        ev.local_port = localPort;
        ev.remote_ep = remoteEP;
        netlog_socket_event(&extraOpts, &ev);
        while (r_head != r_tail) {
            rx_bytes -= ring[r_head].size;
            release((void*)ring[r_head].ptr);
            r_head = (r_head + 1) % UDP_RING_CAP;
        }
        return Socket::close();
    }

    net_l4_endpoint get_remote_ep() const {
        return remoteEP;
    }
};