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
#include "networking/network.h"
#include "networking/interface_manager.h"
#include "exceptions/irq.h"
#include "sysregs.h"

static constexpr uint32_t UDP_DEFAULT_RING_CAP = 64;
static constexpr uint32_t UDP_MAX_RING_CAP = 1024;

class UDPSocket : public Socket {
    netpkt_t** ring = nullptr;
    net_l4_endpoint* src_eps = nullptr;
    net_l4_endpoint* mcast_groups = nullptr;
    uint8_t mcast_count = 0;
    uint32_t ring_cap = 0;
    uint32_t r_head = 0;
    uint32_t r_tail = 0;
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
                if (memcmp(group->ip, dst_ip_addr, 16) == 0) return true;
            }
        }

        return false;
    }

    void on_receive(ip_version_t ver, const void* src_ip_addr, uint16_t src_port, netpkt_t* pkt) {
        if (!pkt) return;
        uint32_t len = netpkt_len(pkt);
        uint32_t limit = 0xFFFFFFFFu;
        if ((extraOpts.flags & SOCK_OPT_BUF_SIZE) && extraOpts.buf_size) limit = extraOpts.buf_size;
        if (len > limit) return;
        if (rx_bytes > limit - len) return;

        if (!ring || !src_eps || !ring_cap) {
            uint32_t usable = UDP_DEFAULT_RING_CAP;
            if ((extraOpts.flags & SOCK_OPT_BUF_SIZE) && extraOpts.buf_size) {
                usable = extraOpts.buf_size / MAX_PACKET_SIZE;
                if (usable < 4) usable = 4;
                if (usable > UDP_MAX_RING_CAP) usable = UDP_MAX_RING_CAP;
            }

            ring_cap = usable + 1;
            ring = (netpkt_t**)zalloc(sizeof(netpkt_t*) * ring_cap);
            src_eps = (net_l4_endpoint*)zalloc(sizeof(net_l4_endpoint) * ring_cap);
            if (!ring || !src_eps) {
                if (ring) release(ring);
                if (src_eps) release(src_eps);
                ring = nullptr;
                src_eps = nullptr;
                ring_cap = 0;
                return;
            }
        }

        uint32_t nexti = (r_tail + 1) % ring_cap;
        if (nexti == r_head) return;

        netpkt_ref(pkt);
        ring[r_tail] = pkt;
        rx_bytes += len;

        src_eps[r_tail].ver = ver;
        memset(src_eps[r_tail].ip, 0, 16);

        if (ver == IP_VER4) memcpy(src_eps[r_tail].ip, src_ip_addr, 4);
        else if (ver == IP_VER6) memcpy(src_eps[r_tail].ip, src_ip_addr, 16);

        src_eps[r_tail].port = src_port;
        r_tail = nexti;
    }

    void leave_mcast_groups() {
        if (!mcast_groups || !mcast_count || !localPort) return;

        uint8_t ids[MAX_L3_INTERFACES];
        for (uint32_t gidx = 0; gidx < mcast_count; ++gidx) {
            const net_l4_endpoint* group = &mcast_groups[gidx];

            if (group->ver == IP_VER4) {
                uint32_t g = 0;
                memcpy(&g, group->ip, 4);
                if (!ipv4_is_multicast(g)) continue;

                uint32_t n = socket_bind_select_l3(&bindSpec, IP_VER4, ids, MAX_IPV4_L3_INTERFACES);
                for (uint32_t i = 0; i < n; ++i) {
                    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(ids[i]);
                    if (!is_valid_v4_l3_for_bind(v4)) continue;
                    l2_ipv4_mcast_leave(v4->l2->ifindex,g);
                }
            } else if (group->ver == IP_VER6) {
                if (!ipv6_is_multicast(group->ip)) continue;
                bool linkscope = ipv6_is_linkscope_mcast(group->ip);

                uint32_t n = socket_bind_select_l3(&bindSpec, IP_VER6, ids, MAX_IPV6_L3_INTERFACES);
                for (uint32_t i = 0; i < n; ++i) {
                    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(ids[i]);
                    if (!is_valid_v6_l3_for_bind(v6)) continue;
                    if (linkscope && !ipv6_is_linklocal(v6->ip)) continue;
                    l2_ipv6_mcast_leave(v6->l2->ifindex,group->ip);
                }
            }
        }
    }

    int32_t join_mcast_groups() {
        if (!mcast_groups || !mcast_count) return SOCK_OK;
        if (!localPort) return SOCK_OK;

        uint8_t ids[MAX_L3_INTERFACES];
        for (uint32_t gidx = 0; gidx < mcast_count; ++gidx) {
            const net_l4_endpoint* group = &mcast_groups[gidx];
            uint32_t n = 0;
            bool joined = false;

            if (group->ver == IP_VER4) {
                uint32_t g = 0;
                memcpy(&g, group->ip, 4);
                if (!ipv4_is_multicast(g)) return SOCK_ERR_INVAL;
                n = socket_bind_select_l3(&bindSpec, IP_VER4, ids, MAX_IPV4_L3_INTERFACES);
                for (uint32_t i = 0; i < n; ++i) {
                    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(ids[i]);
                    if (!is_valid_v4_l3_for_bind(v4)) continue;
                    if (l2_ipv4_mcast_join(v4->l2->ifindex, g)) joined = true;
                }
            } else if (group->ver == IP_VER6) {
                if (!ipv6_is_multicast(group->ip)) return SOCK_ERR_INVAL;
                bool linkscope = ipv6_is_linkscope_mcast(group->ip);
                n = socket_bind_select_l3(&bindSpec, IP_VER6, ids, MAX_IPV6_L3_INTERFACES);
                for (uint32_t i = 0; i < n; ++i) {
                    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(ids[i]);
                    if (!is_valid_v6_l3_for_bind(v6)) continue;
                    if (linkscope && !ipv6_is_linklocal(v6->ip)) continue;
                    if (l2_ipv6_mcast_join(v6->l2->ifindex, group->ip)) joined = true;
                }
            } else return SOCK_ERR_INVAL;

            if (!joined) return SOCK_ERR_SYS;
        }

        return SOCK_OK;
    }

public:
    UDPSocket(ksocket_t* owner, const SocketExtraOptions* extra = nullptr) : Socket(owner, extra) {
        extraOpts.flags &= ~(SOCK_OPT_MCAST_JOIN | SOCK_OPT_MCAST_LEAVE);
        extraOpts.mcast_count = 0;
        extraOpts.mcast_groups = nullptr;
    }

    ~UDPSocket() override {
        close();
    }

    int32_t bind(const SockBindSpec& spec_in, uint16_t port) override {
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_UDP;
        ev.action = NETLOG_ACT_BIND;
        ev.pid = socket_core_pid(ownerSocket);
        ev.u0 = port;
        ev.bind_spec = spec_in;
        netlog_socket_event(&extraOpts, &ev);
        if (localPort) return SOCK_ERR_BOUND;
        if (!ownerSocket) return SOCK_ERR_SYS;

        SockBindSpec spec = spec_in;
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
            bind_port = socket_bind_alloc_ephemeral(ownerSocket, PROTO_UDP, &spec, &token);
            if (bind_port < 0) return SOCK_ERR_NO_PORT;
        } else if (!socket_bind_insert(ownerSocket, PROTO_UDP, &spec, port, &token)) return SOCK_ERR_BOUND;

        bindSpec = spec;
        bindToken = token;
        localPort = (uint16_t)bind_port;

        int32_t mcast_res = join_mcast_groups();
        if (mcast_res != SOCK_OK) {
            leave_mcast_groups();
            socket_bind_remove(bindToken);
            localPort = 0;
            memset(&bindSpec, 0, sizeof(bindSpec));
            bindToken = 0;
            return mcast_res;
        }

        return SOCK_OK;
    }

    int32_t connect(const net_l4_endpoint* dst) {
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_UDP;
        ev.action = NETLOG_ACT_CONNECT;
        ev.pid = socket_core_pid(ownerSocket);
        if (dst) ev.dst_ep = *dst;
        netlog_socket_event(&extraOpts, &ev);

        if (!dst || !dst->port) return SOCK_ERR_INVAL;
        if (dst->ver != IP_VER4 && dst->ver != IP_VER6) return SOCK_ERR_INVAL;

        remoteEP = *dst;
        connected = true;
        return SOCK_OK;
    }

    int64_t sendto(const net_l4_endpoint* dst, const void* buf, uint64_t len) {
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_UDP;
        ev.action = NETLOG_ACT_SENDTO;
        ev.pid = socket_core_pid(ownerSocket);
        ev.u1 = (uint32_t)len;
        if (dst) {
            ev.dst_ep = *dst;
            ev.u0 = dst->port;
        }
        netlog_socket_event(&extraOpts, &ev);
        bool explicit_dst = dst != nullptr;
        if (!dst) {
            if (!connected) return SOCK_ERR_STATE;
            dst = &remoteEP;
        }
        if (!dst->port) return SOCK_ERR_INVAL;
        if (!buf && len) return SOCK_ERR_INVAL;
        if (!ownerSocket) return SOCK_ERR_SYS;

        net_l4_endpoint d = *dst;
        if (connected && explicit_dst) {
            if (d.ver != remoteEP.ver || d.port != remoteEP.port) return SOCK_ERR_STATE;
            if (d.ver == IP_VER4 && memcmp(d.ip, remoteEP.ip, 4) != 0) return SOCK_ERR_STATE;
            if (d.ver == IP_VER6 && memcmp(d.ip, remoteEP.ip, 16) != 0) return SOCK_ERR_STATE;
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
            l3_ipv4_interface_t* bcast_v4 = nullptr;

            if (limited_bcast) {
                if (!(extraOpts.flags & SOCK_OPT_BROADCAST_ALLOWED)) return SOCK_ERR_PERM;

                uint32_t n = socket_bind_select_l3(&bindSpec, IP_VER4, bcast_ids, MAX_IPV4_L3_INTERFACES);
                uint32_t valid = 0;
                for (uint32_t i = 0; i < n; ++i) {
                    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(bcast_ids[i]);
                    if (!is_valid_v4_l3_for_bind(v4) || v4->is_localhost) continue;

                    if (!v4->ip && v4->mode != IPV4_CFG_DHCP) continue;
                    if (!valid) {
                        chosen_l3 = bcast_ids[i];
                        bcast_v4 = v4;
                    }
                    valid++;
                }
                if (valid != 1 || !bcast_v4) return SOCK_ERR_INVAL;
            } else {
                uint32_t n = socket_bind_select_l3(&bindSpec, IP_VER4, bcast_ids, MAX_IPV4_L3_INTERFACES);
                for (uint32_t i = 0; i < n; ++i) {
                    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(bcast_ids[i]);
                    if (!is_valid_v4_l3_for_bind(v4) || !v4->ip || !v4->mask || v4->is_localhost) continue;
                    if (ipv4_broadcast_calc(v4->ip, v4->mask) != dip) continue;
                    chosen_l3 = bcast_ids[i];
                    bcast_v4 = v4;
                    break;
                }
                if (bcast_v4 && !(extraOpts.flags & SOCK_OPT_BROADCAST_ALLOWED)) return SOCK_ERR_PERM;
            }

            if (bcast_v4) {
                if (!localPort) {
                    socket_bind_token_t token = 0;
                    int p = socket_bind_alloc_ephemeral_l3(ownerSocket, PROTO_UDP, chosen_l3, &token);
                    if (p < 0) return SOCK_ERR_NO_PORT;
                    localPort = (uint16_t)p;
                    bindToken = token;
                    memset(&bindSpec, 0, sizeof(bindSpec));
                    bindSpec.kind = BIND_L3;
                    bindSpec.ver = IP_VER4;
                    bindSpec.l3_id = chosen_l3;
                }

                net_l4_endpoint src;
                src.ver = IP_VER4;
                memset(src.ip, 0, 16);
                memcpy(src.ip, &bcast_v4->ip, 4);
                src.port = localPort;

                ip_tx_opts_t tx;
                tx.scope = IP_TX_BOUND_L3;
                tx.index = chosen_l3;

                udp_send_segment(&src, &d, pay, &tx, (extraOpts.flags & SOCK_OPT_TTL) ? extraOpts.ttl : 0, (extraOpts.flags & SOCK_OPT_DONTFRAG) ? 1 : 0);
                return (int64_t)len;
            }

            ipv4_tx_plan_t plan;
            if (!socket_bind_build_ipv4_tx_plan(&bindSpec, localPort, dip, &plan)) return SOCK_ERR_SYS;

            uint8_t tx_l3 = plan.l3_id;
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(tx_l3);
            if (!is_valid_v4_l3_for_bind(v4)) return SOCK_ERR_SYS;

            if (!localPort) {
                socket_bind_token_t token = 0;
                int p = socket_bind_alloc_ephemeral_l3(ownerSocket, PROTO_UDP, tx_l3, &token);
                if (p < 0) return SOCK_ERR_NO_PORT;
                localPort = (uint16_t)p;
                bindToken = token;
                memset(&bindSpec, 0, sizeof(bindSpec));
                bindSpec.kind = BIND_L3;
                bindSpec.ver = IP_VER4;
                bindSpec.l3_id = tx_l3;
            }

            net_l4_endpoint src;
            src.ver = IP_VER4;
            memset(src.ip, 0, 16);
            memcpy(src.ip, &v4->ip, 4);
            src.port = localPort;

            ip_tx_opts_t tx;
            tx.scope = IP_TX_BOUND_L3;
            tx.index = plan.l3_id;

            udp_send_segment(&src, &d, pay, &tx, (extraOpts.flags & SOCK_OPT_TTL) ? extraOpts.ttl : 0, (extraOpts.flags & SOCK_OPT_DONTFRAG) ? 1 : 0);
            return (int64_t)len;
        }

        if (d.ver == IP_VER6) {
            ipv6_tx_plan_t plan;
            if (!socket_bind_build_ipv6_tx_plan(&bindSpec, localPort, d.ip, &plan)) return SOCK_ERR_SYS;

            uint8_t chosen_l3 = plan.l3_id;

            l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(chosen_l3);
            if (!is_valid_v6_l3_for_bind(v6)) return SOCK_ERR_SYS;

            if (!localPort) {
                socket_bind_token_t token = 0;
                int p = socket_bind_alloc_ephemeral_l3(ownerSocket, PROTO_UDP, chosen_l3, &token);
                if (p < 0) return SOCK_ERR_NO_PORT;
                localPort = (uint16_t)p;
                bindToken = token;
                memset(&bindSpec, 0, sizeof(bindSpec));
                bindSpec.kind = BIND_L3;
                bindSpec.ver = IP_VER6;
                bindSpec.l3_id = chosen_l3;
            }

            net_l4_endpoint src;
            src.ver = IP_VER6;
            memset(src.ip, 0, 16);
            memcpy(src.ip, v6->ip, 16);
            src.port = localPort;

            ip_tx_opts_t tx;
            tx.scope = IP_TX_BOUND_L3;
            tx.index = plan.l3_id;

            udp_send_segment(&src, &d, pay, &tx, (extraOpts.flags & SOCK_OPT_TTL) ? extraOpts.ttl : 0, (extraOpts.flags & SOCK_OPT_DONTFRAG) ? 1 : 0);
            return (int64_t)len;
        }

        return SOCK_ERR_INVAL;
    }

    uint32_t enqueue_datagram(ip_version_t ipver, const void* src_ip_addr, const void* dst_ip_addr, netpkt_t* pkt, uint16_t src_port, uint16_t dst_port) {
        if (!bindToken || localPort != dst_port || !pkt || !dst_ip_addr || !src_ip_addr) return 0;
        if (ipver == IP_VER4) {
            uint32_t dip = 0;
            memcpy(&dip, dst_ip_addr, 4);
            if (ipv4_is_multicast(dip) && !udp_mcast_match(this, IP_VER4, dst_ip_addr)) return 0;
        } else if (ipver == IP_VER6) {
            if (ipv6_is_multicast((const uint8_t*)dst_ip_addr) && !udp_mcast_match(this, IP_VER6, dst_ip_addr)) return 0;
        }
        if (connected) {
            if (remoteEP.ver != ipver || remoteEP.port != src_port) return 0;
            if (ipver == IP_VER4 && memcmp(remoteEP.ip, src_ip_addr, 4) != 0) return 0;
            if (ipver == IP_VER6 && memcmp(remoteEP.ip, src_ip_addr, 16) != 0) return 0;
        }

        on_receive(ipver, src_ip_addr, src_port, pkt);
        return netpkt_len(pkt);
    }

    int64_t recvfrom(void* buf, uint64_t len, net_l4_endpoint* src) {
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_UDP;
        ev.action = NETLOG_ACT_RECVFROM;
        ev.pid = socket_core_pid(ownerSocket);
        ev.u0 = (uint32_t)len;
        ev.local_port = localPort;
        ev.remote_ep = remoteEP;
        netlog_socket_event(&extraOpts, &ev);
        if (!ring || r_head == r_tail) return SOCK_ERR_WOULDBLOCK;

        netpkt_t* p = ring[r_head];
        net_l4_endpoint se = src_eps[r_head];
        ring[r_head] = nullptr;
        r_head = (r_head + 1) % ring_cap;

        uint32_t pkt_len = p ? netpkt_len(p) : 0;
        rx_bytes -= pkt_len;
        uint32_t tocpy = pkt_len;
        if (tocpy > len) tocpy = (uint32_t)len;

        if (tocpy && !netpkt_copyout(p, 0, buf, tocpy)) tocpy = 0;
        if (src) *src = se;

        if (p) netpkt_unref(p);
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
                uint32_t count32 = len / sizeof(net_l4_endpoint);
                if (!count32 || count32 > 255 || count32 + mcast_count > 255) return SOCK_ERR_INVAL;

                const net_l4_endpoint* groups = (const net_l4_endpoint*)value;
                for (uint32_t i = 0; i < count32; ++i) {
                    if (groups[i].ver != IP_VER4 && groups[i].ver != IP_VER6) return SOCK_ERR_INVAL;
                    if (groups[i].ver == IP_VER4) {
                        uint32_t ip = 0;
                        memcpy(&ip, groups[i].ip, 4);
                        if (!ipv4_is_multicast(ip)) return SOCK_ERR_INVAL;
                    } else if (!ipv6_is_multicast(groups[i].ip)) return SOCK_ERR_INVAL;
                }

                net_l4_endpoint* next = (net_l4_endpoint*)zalloc(sizeof(net_l4_endpoint) * (mcast_count + count32));
                if (!next) return SOCK_ERR_SYS;
                if (mcast_count) memcpy(next, mcast_groups, sizeof(net_l4_endpoint) * mcast_count);

                uint8_t next_count = mcast_count;
                for (uint32_t i = 0; i < count32; ++i) {
                    bool exists = false;
                    for (uint8_t j = 0; j < next_count; ++j) {
                        if (next[j].ver != groups[i].ver || next[j].port != groups[i].port) continue;
                        uint32_t sz = groups[i].ver == IP_VER4 ? 4 : 16;
                        if (memcmp(next[j].ip, groups[i].ip, sz) == 0) exists = true;
                    }
                    if (!exists) next[next_count++] = groups[i];
                }

                if (next_count == mcast_count) {
                    release(next);
                    return SOCK_OK;
                }

                net_l4_endpoint* old_groups = mcast_groups;
                uint8_t old_count = mcast_count;
                if (localPort) leave_mcast_groups();

                mcast_groups = next;
                mcast_count = next_count;
                int32_t rc = join_mcast_groups();
                if (rc != SOCK_OK) {
                    leave_mcast_groups();
                    release(next);
                    mcast_groups = old_groups;
                    mcast_count = old_count;
                    extraOpts.mcast_groups = mcast_groups;
                    extraOpts.mcast_count = mcast_count;
                    if (mcast_count) extraOpts.flags |= SOCK_OPT_MCAST_JOIN;
                    else extraOpts.flags &= ~SOCK_OPT_MCAST_JOIN;
                    if (localPort) join_mcast_groups();
                    return rc;
                }

                if (old_groups) release(old_groups);
                extraOpts.mcast_groups = mcast_groups;
                extraOpts.mcast_count = mcast_count;
                extraOpts.flags |= SOCK_OPT_MCAST_JOIN;
                return SOCK_OK;
            }
            case SOCK_OPT_MCAST_LEAVE: {
                if (!value || !len || (len % sizeof(net_l4_endpoint)) != 0) return SOCK_ERR_INVAL;
                uint32_t count = len / sizeof(net_l4_endpoint);
                if (!count || count > 255) return SOCK_ERR_INVAL;
                if (!mcast_groups || !mcast_count) return SOCK_OK;

                const net_l4_endpoint* groups = (const net_l4_endpoint*)value;
                for (uint32_t i = 0; i < count; ++i) {
                    if (groups[i].ver != IP_VER4 && groups[i].ver != IP_VER6) return SOCK_ERR_INVAL;
                    if (groups[i].ver == IP_VER4) {
                        uint32_t ip = 0;
                        memcpy(&ip, groups[i].ip, 4);
                        if (!ipv4_is_multicast(ip)) return SOCK_ERR_INVAL;
                    } else if (!ipv6_is_multicast(groups[i].ip)) return SOCK_ERR_INVAL;
                }

                net_l4_endpoint* next = nullptr;
                if (mcast_count > count) {
                    next = (net_l4_endpoint*)zalloc(sizeof(net_l4_endpoint) * mcast_count);
                    if (!next) return SOCK_ERR_SYS;
                }

                uint8_t next_count = 0;
                for (uint8_t i = 0; i < mcast_count; ++i) {
                    bool remove_group = false;
                    for (uint32_t j = 0; j < count; ++j) {
                        if (mcast_groups[i].ver != groups[j].ver || mcast_groups[i].port != groups[j].port) continue;
                        uint32_t sz = groups[j].ver == IP_VER4 ? 4 : 16;
                        if (memcmp(mcast_groups[i].ip, groups[j].ip, sz) == 0) remove_group = true;
                    }
                    if (!remove_group) {
                        if (!next) {
                            next = (net_l4_endpoint*)zalloc(sizeof(net_l4_endpoint) * mcast_count);
                            if (!next) return SOCK_ERR_SYS;
                        }
                        next[next_count++] = mcast_groups[i];
                    }
                }

                if (next_count == mcast_count) {
                    if (next) release(next);
                    return SOCK_OK;
                }

                if (!next_count && next) {
                    release(next);
                    next = nullptr;
                }

                net_l4_endpoint* old_groups = mcast_groups;
                uint8_t old_count = mcast_count;
                if (localPort) leave_mcast_groups();

                mcast_groups = next;
                mcast_count = next_count;
                int32_t rc = join_mcast_groups();
                if (rc != SOCK_OK) {
                    leave_mcast_groups();
                    if (next) release(next);
                    mcast_groups = old_groups;
                    mcast_count = old_count;
                    extraOpts.mcast_groups = mcast_groups;
                    extraOpts.mcast_count = mcast_count;
                    extraOpts.flags |= SOCK_OPT_MCAST_JOIN;
                    if (localPort) join_mcast_groups();
                    return rc;
                }

                release(old_groups);
                extraOpts.mcast_count = mcast_count;
                extraOpts.mcast_groups = mcast_groups;
                if (mcast_count) extraOpts.flags |= SOCK_OPT_MCAST_JOIN;
                else extraOpts.flags &= ~SOCK_OPT_MCAST_JOIN;
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
                if (!len) return SOCK_ERR_INVAL;
                uint32_t need = mcast_count * sizeof(net_l4_endpoint);
                if (!value) {
                    *len = need;
                    return SOCK_OK;
                }
                if (*len < need) return SOCK_ERR_INVAL;
                if (need) memcpy(value, mcast_groups, need);
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
        ev.pid = socket_core_pid(ownerSocket);
        ev.local_port = localPort;
        ev.remote_ep = remoteEP;
        netlog_socket_event(&extraOpts, &ev);
        if (ring) {
            while (r_head != r_tail) {
                if (ring[r_head]) {
                    rx_bytes -= netpkt_len(ring[r_head]);
                    netpkt_unref(ring[r_head]);
                }
                r_head = (r_head + 1) % ring_cap;
            }
            release(ring);
            ring = nullptr;
        }
        if (src_eps) {
            release(src_eps);
            src_eps = nullptr;
        }
        if (mcast_groups) {
            release(mcast_groups);
            mcast_groups = nullptr;
        }
        mcast_count = 0;
        extraOpts.mcast_count = 0;
        extraOpts.mcast_groups = nullptr;
        extraOpts.flags &= ~(SOCK_OPT_MCAST_JOIN | SOCK_OPT_MCAST_LEAVE);
        ring_cap = 0;
        r_head = 0;
        r_tail = 0;
        rx_bytes = 0;
        return Socket::close();
    }

    net_l4_endpoint get_remote_ep() const {
        return remoteEP;
    }
};