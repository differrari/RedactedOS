#pragma once
#include "socket.hpp"
#include "networking/transport_layer/udp.h"
#include "types.h"
#include "std/memory.h"
#include "networking/application_layer/dns/dns.h"
#include "net/socket_types.h"
#include "networking/internet_layer/ipv4_route.h"
#include "networking/internet_layer/ipv6_route.h"
#include "syscalls/syscalls.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/igmp.h"

static constexpr int32_t UDP_RING_CAP = 1024;
static constexpr dns_server_sel_t UDP_DNS_SEL = DNS_USE_BOTH;
static constexpr uint32_t UDP_DNS_TIMEOUT_MS = 3000;

class UDPSocket : public Socket {
    inline static UDPSocket* s_list_head = nullptr;

    sizedptr ring[UDP_RING_CAP];
    net_l4_endpoint src_eps[UDP_RING_CAP];
    int32_t r_head = 0;
    int32_t r_tail = 0;
    uint32_t rx_bytes = 0;

    UDPSocket* next = nullptr;

    static bool is_valid_v4_l3_for_bind(l3_ipv4_interface_t* v4) {
        if (!v4) return false;
        if (!v4->l2) return false;
        if (!v4->l2->is_up) return false;
        if (v4->mode == IPV4_CFG_DISABLED) return false;
        if (!v4->port_manager) return false;
        return true;
    }

    static bool is_valid_v6_l3_for_bind(l3_ipv6_interface_t* v6) {
        if (!v6) return false;
        if (!v6->l2) return false;
        if (!v6->l2->is_up) return false;
        if (v6->cfg == IPV6_CFG_DISABLE) return false;
        if (v6->dad_state != IPV6_DAD_OK) return false;
        if (!v6->port_manager) return false;
        return true;
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

    static bool socket_matches_dst(UDPSocket* s, uint8_t ifx, ip_version_t ver, const void* dst_ip_addr, uint16_t dst_port) {
        if (!s) return false;
        if (!s->bound) return false;
        if (s->localPort != dst_port) return false;
        if (!dst_ip_addr) return false;

        if (ver == IP_VER4) {
            uint32_t dip = *(const uint32_t*)dst_ip_addr;
            bool lb = dip == 0xFFFFFFFFu;
            bool mc = ipv4_is_multicast(dip);

            for (int i = 0; i < s->bound_l3_count; ++i) {
                uint8_t id = s->bound_l3[i];
                l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(id);
                if (!is_valid_v4_l3_for_bind(v4)) continue;
                if (v4->l2->ifindex != ifx) continue;
                if (lb) return true;
                if (mc) return true;

                if (v4->mask) {
                    uint32_t b = ipv4_broadcast_calc(v4->ip, v4->mask);
                    if (b == dip)return true;
                }

                if (v4->ip == dip) return true;
            }
            return false;
        }

        if (ver == IP_VER6) {
            bool mcast = ipv6_is_multicast((const uint8_t*)dst_ip_addr);

            for (int i = 0; i < s->bound_l3_count; ++i) {
                uint8_t id = s->bound_l3[i];
                l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(id);
                if (!is_valid_v6_l3_for_bind(v6)) continue;
                if (v6->l2->ifindex != ifx) continue;

                if (mcast) return true;
                if (memcmp(v6->ip, dst_ip_addr, 16) == 0) return true;
            }
            return false;
        }

        return false;
    }

    static uint32_t dispatch(uint8_t ifindex, ip_version_t ipver, const void* src_ip_addr, const void* dst_ip_addr, uintptr_t frame_ptr, uint32_t frame_len, uint16_t src_port, uint16_t dst_port) {

        for (UDPSocket* s = s_list_head; s; s = s->next) {
            if (!socket_matches_dst(s, ifindex, ipver, dst_ip_addr, dst_port))
                continue;

            uintptr_t copy = (uintptr_t)malloc(frame_len);
            if (!copy) continue;

            memcpy((void*)copy, (const void*)frame_ptr, frame_len);
            s->on_receive(ipver, src_ip_addr, src_port, copy, frame_len);
        }
        if (frame_ptr && frame_len) free_sized((void*)frame_ptr, frame_len);
        return frame_len;
    }

    void on_receive(ip_version_t ver, const void* src_ip_addr, uint16_t src_port, uintptr_t ptr, uint32_t len) {
        uint32_t limit = 0xFFFFFFFFu;
        if ((extraOpts.flags & SOCK_OPT_BUF_SIZE) && extraOpts.buf_size) limit = extraOpts.buf_size;
        if (len > limit) {
            if (ptr && len) free_sized((void*)ptr, len);
            return;
        }

        while (rx_bytes + len > limit && r_head != r_tail) {
            rx_bytes -= ring[r_head].size;
            free_sized((void*)ring[r_head].ptr, ring[r_head].size);
            r_head = (r_head + 1) % UDP_RING_CAP;
        }
        uintptr_t copy = (uintptr_t)malloc(len);
        if (!copy) {
            if (ptr && len) free_sized((void*)ptr, len);
            return;
        }

        memcpy((void*)copy, (void*)ptr, len);
        if (ptr && len) free_sized((void*)ptr, len);

        int nexti = (r_tail + 1) % UDP_RING_CAP;
        if (nexti == r_head) {
            rx_bytes -= ring[r_head].size;
            free_sized((void*)ring[r_head].ptr, ring[r_head].size);
            r_head = (r_head + 1) % UDP_RING_CAP;
        }

        ring[r_tail].ptr = copy;
        ring[r_tail].size = len;
        rx_bytes += len;

        src_eps[r_tail].ver = ver;
        memset(src_eps[r_tail].ip, 0, 16);

        if (ver == IP_VER4) {
            uint32_t v4 = *(const uint32_t*)src_ip_addr;
            memcpy(src_eps[r_tail].ip, &v4, 4);
        } else if (ver == IP_VER6) {
            memcpy(src_eps[r_tail].ip, src_ip_addr, 16);
        }

        src_eps[r_tail].port = src_port;

        r_tail = nexti;
        remoteEP = src_eps[(r_tail + UDP_RING_CAP - 1) % UDP_RING_CAP];
    }

    void insert_in_list() {
        next = s_list_head;
        s_list_head = this;
    }

    void remove_from_list() {
        UDPSocket** cur = &s_list_head;
        while (*cur) {
            if (*cur == this) {
                *cur = (*cur)->next;
                break;
            }
            cur = &((*cur)->next);
        }
        next = nullptr;
    }

    bool add_all_l3_on_l2(uint8_t ifindex, uint8_t* tmp_ids, int& n) {
        l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
        if (!l2) return false;
        if (!l2->is_up) return false;

        for (int s = 0; s < MAX_IPV4_PER_INTERFACE; ++s) {
            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
            if (!is_valid_v4_l3_for_bind(v4)) continue;
            if (n < SOCK_MAX_L3) tmp_ids[n++] = v4->l3_id;
        }

        for (int s = 0; s < MAX_IPV6_PER_INTERFACE; ++s) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[s];
            if (!is_valid_v6_l3_for_bind(v6)) continue;
            if (n < SOCK_MAX_L3) tmp_ids[n++] = v6->l3_id;
        }

        return n > 0;
    }

    bool add_all_l3_any(uint8_t* tmp_ids, int& n) {
        uint8_t cnt = l2_interface_count();
        for (uint8_t i = 0; i < cnt; ++i) {
            l2_interface_t* l2 = l2_interface_at(i);
            if (!l2) continue;
            if (!l2->is_up) continue;

            for (int s = 0; s < MAX_IPV4_PER_INTERFACE; ++s) {
                l3_ipv4_interface_t* v4 = l2->l3_v4[s];
                if (!is_valid_v4_l3_for_bind(v4)) continue;
                if (n < SOCK_MAX_L3) tmp_ids[n++] = v4->l3_id;
            }

            for (int s = 0; s < MAX_IPV6_PER_INTERFACE; ++s) {
                l3_ipv6_interface_t* v6 = l2->l3_v6[s];
                if (!is_valid_v6_l3_for_bind(v6)) continue;
                if (n < SOCK_MAX_L3) tmp_ids[n++] = v6->l3_id;
            }
        }
        return n > 0;
    }

    void do_unbind_one(uint8_t l3_id, uint16_t port, uint16_t pid_) override {
        (void)pid_;
        udp_unbind_l3(l3_id, port, pid);
    }

    static bool pick_v4_l3_for_unicast(uint32_t dip, const uint8_t* candidates, int n, uint8_t* out_l3) {
        if (!out_l3) return false;
        ipv4_tx_plan_t plan;
        if (!ipv4_build_tx_plan(dip, nullptr, candidates, n, &plan)) return false;
        *out_l3 = plan.l3_id;
        return true;
    }

    static bool pick_v6_l3_for_unicast(const uint8_t dst_ip[16], const uint8_t* candidates, int n, uint8_t* out_l3) {
        if (!out_l3) return false;
        ipv6_tx_plan_t plan;
        if (!ipv6_build_tx_plan(dst_ip, nullptr, candidates, n, &plan)) return false;
        *out_l3 = plan.l3_id;
        return true;
    }

public:
    UDPSocket(uint8_t r, uint32_t pid_, const SocketExtraOptions* extra = nullptr) : Socket(PROTO_UDP, r, extra) {
        pid = pid_;
        insert_in_list();
    }

    ~UDPSocket() override {
        if ((extraOpts.flags & SOCK_OPT_MCAST_JOIN) && extraOpts.mcast_ver) {
            if (extraOpts.mcast_ver == IP_VER4) {
                uint32_t g = 0;
                memcpy(&g, extraOpts.mcast_group, 4);
                if (ipv4_is_multicast(g)) {
                    for (int i = 0; i < bound_l3_count; ++i) {
                        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(bound_l3[i]);
                        if (!is_valid_v4_l3_for_bind(v4)) continue;
                        if (!v4->l2) continue;
                        (void)l2_ipv4_mcast_leave(v4->l2->ifindex, g);
                    }
                }
            } else if (extraOpts.mcast_ver == IP_VER6) {
                if (ipv6_is_multicast(extraOpts.mcast_group)) {
                    for (int i = 0; i < bound_l3_count; ++i) {
                        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(bound_l3[i]);
                        if (!is_valid_v6_l3_for_bind(v6)) continue;
                        if (!v6->l2) continue;
                        (void)l2_ipv6_mcast_leave(v6->l2->ifindex, extraOpts.mcast_group);
                    }
                }
            }
        }
        close();
        remove_from_list();
    }

    int32_t bind(const SockBindSpec& spec_in, uint16_t port) override {
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_UDP;
        ev.action = NETLOG_ACT_BIND;
        ev.pid = pid;
        ev.u0 = port;
        ev.bind_spec = spec_in;
        netlog_socket_event(&extraOpts, &ev);
        if (role != SOCK_ROLE_SERVER) return SOCK_ERR_PERM;
        if (bound) return SOCK_ERR_BOUND;

        SockBindSpec spec = spec_in;
        bool empty = spec.kind == BIND_L3 && spec.l3_id == 0 && spec.ifindex == 0 && spec.ver == 0 && ipv6_is_unspecified(spec.ip);
        if (empty) spec.kind = BIND_ANY;

        uint8_t ids[SOCK_MAX_L3];
        int n = 0;

        if (spec.kind == BIND_L3) {
            if (!spec.l3_id) return SOCK_ERR_INVAL;

            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(spec.l3_id);
            l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(spec.l3_id);

            bool ok4 = is_valid_v4_l3_for_bind(v4);
            bool ok6 = is_valid_v6_l3_for_bind(v6);

            if (!ok4 && !ok6) return SOCK_ERR_INVAL;

            if (ok4 && n < SOCK_MAX_L3) ids[n++] = spec.l3_id;
            if (ok6 && n < SOCK_MAX_L3) ids[n++] = spec.l3_id;
        } else if (spec.kind == BIND_L2) {
            if (!add_all_l3_on_l2(spec.ifindex, ids, n)) return SOCK_ERR_INVAL;
        } else if (spec.kind == BIND_IP) {
            if (spec.ver == IP_VER4) {
                uint32_t v4ip = 0;
                memcpy(&v4ip, spec.ip, 4);
                l3_ipv4_interface_t* ipif = l3_ipv4_find_by_ip(v4ip);
                if (!is_valid_v4_l3_for_bind(ipif)) return SOCK_ERR_INVAL;
                if (n < SOCK_MAX_L3) ids[n++] = ipif->l3_id;
            } else if (spec.ver == IP_VER6) {
                l3_ipv6_interface_t* ipif6 = l3_ipv6_find_by_ip(spec.ip);
                if (!is_valid_v6_l3_for_bind(ipif6)) return SOCK_ERR_INVAL;
                if (n < SOCK_MAX_L3) ids[n++] = ipif6->l3_id;
            } else {
                return SOCK_ERR_INVAL;
            }
        } else if (spec.kind == BIND_ANY) {
            if (!add_all_l3_any(ids, n)) return SOCK_ERR_INVAL;
        } else {
            return SOCK_ERR_INVAL;
        }

        if (n == 0) return SOCK_ERR_INVAL;

        uint8_t dedup[SOCK_MAX_L3];
        int m = 0;
        for (int i = 0; i < n; ++i) {
            bool seen = false;
            for (int j = 0; j < m; ++j) {
                if (dedup[j] == ids[i]) {
                    seen = true;
                    break;
                }
            }
            if (!seen && m < SOCK_MAX_L3) dedup[m++] = ids[i];
        }
        if (m == 0) return SOCK_ERR_INVAL;

        int bdone = 0;
        for (int i = 0; i < m; ++i) {
            uint8_t id = dedup[i];
            if (udp_bind_l3(id, port, pid, dispatch)) {
                bdone++;
                continue;
            }
            for (int j = 0; j < bdone; ++j) udp_unbind_l3(dedup[j], port, pid);
            return SOCK_ERR_SYS;
        }

        clear_bound_l3();
        for (int i = 0; i < m; ++i) add_bound_l3(dedup[i]);

        if ((extraOpts.flags & SOCK_OPT_MCAST_JOIN) && extraOpts.mcast_ver) {
            if (extraOpts.mcast_ver == IP_VER4) {
                uint32_t g = 0;
                memcpy(&g, extraOpts.mcast_group, 4);
                if (ipv4_is_multicast(g)) {
                    for (int i = 0; i < bound_l3_count; ++i) {
                        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(bound_l3[i]);
                        if (!is_valid_v4_l3_for_bind(v4)) continue;
                        if (!v4->l2) continue;
                        (void)l2_ipv4_mcast_join(v4->l2->ifindex, g);
                    }
                }
            } else if (extraOpts.mcast_ver == IP_VER6) {
                if (ipv6_is_multicast(extraOpts.mcast_group)) {
                    for (int i = 0; i < bound_l3_count; ++i) {
                        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(bound_l3[i]);
                        if (!is_valid_v6_l3_for_bind(v6)) continue;
                        if (!v6->l2) continue;
                        (void)l2_ipv6_mcast_join(v6->l2->ifindex, extraOpts.mcast_group);
                    }
                }
            }
        }

        localPort = port;
        bound = true;
        return SOCK_OK;
    }

    int64_t sendto(SockDstKind kind, const void* dst, uint16_t port, const void* buf, uint64_t len) {
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_UDP;
        ev.action = NETLOG_ACT_SENDTO;
        ev.pid = pid;
        ev.dst_kind = kind;
        ev.u0 = port;
        ev.u1 = (uint32_t)len;
        if (kind == DST_ENDPOINT && dst) ev.dst_ep = *(const net_l4_endpoint*)dst;
        if (kind == DST_DOMAIN) ev.s0 = (const char*)dst;
        netlog_socket_event(&extraOpts, &ev);
        if (!dst) return SOCK_ERR_INVAL;
        if (!buf) return SOCK_ERR_INVAL;
        if (len == 0) return SOCK_ERR_INVAL;

        net_l4_endpoint d;
        if (kind == DST_ENDPOINT) {
            const net_l4_endpoint* ed = (const net_l4_endpoint*)dst;
            d = *ed;
            if (!d.port && port) d.port = port;
            if (!d.port) return SOCK_ERR_INVAL;
        } else if (kind == DST_DOMAIN) {
            const char* host = (const char*)dst;
            if (!port) return SOCK_ERR_INVAL;

            uint8_t a6[16];
            memset(a6, 0, 16);
            uint32_t a4 = 0;

            dns_result_t dr6 = dns_resolve_aaaa(host, a6, UDP_DNS_SEL, UDP_DNS_TIMEOUT_MS);
            dns_result_t dr4 = dns_resolve_a(host, &a4, UDP_DNS_SEL, UDP_DNS_TIMEOUT_MS);
            if (dr6 != DNS_OK && dr4 != DNS_OK) return SOCK_ERR_DNS;

            uint8_t allow_v4[SOCK_MAX_L3];
            uint8_t allow_v6[SOCK_MAX_L3];
            int n4 = 0;
            int n6 = 0;
            for (int i = 0; i < bound_l3_count; ++i) {
                uint8_t id = bound_l3[i];
                if (n4 < SOCK_MAX_L3 && l3_ipv4_find_by_id(id)) allow_v4[n4++] = id;
                if (n6 < SOCK_MAX_L3 && l3_ipv6_find_by_id(id)) allow_v6[n6++] = id;
            }

            if (dr6 == DNS_OK) {
                ipv6_tx_plan_t p6;
                if (ipv6_build_tx_plan(a6, nullptr, n6 ? allow_v6 : nullptr, n6, &p6)) {
                    d.ver = IP_VER6;
                    memcpy(d.ip, a6, 16);
                    d.port = port;
                }
            }

            if (d.ver == 0 && dr4 == DNS_OK) {
                ipv4_tx_plan_t p4;
                if (ipv4_build_tx_plan(a4, nullptr, n4 ? allow_v4 : nullptr, n4, &p4)) {
                    make_ep(a4, port, IP_VER4, &d);
                }
            }

            if (d.ver == 0) return SOCK_ERR_SYS;
        } else {
            return SOCK_ERR_INVAL;
        }

        sizedptr pay;
        pay.ptr = (uintptr_t)buf;
        pay.size = (uint32_t)len;
        
        if (d.ver == IP_VER4) {
            uint32_t dip = 0;
            memcpy(&dip, d.ip, 4);

            bool is_bcast = false;
            if (dip == 0xFFFFFFFFu) is_bcast = true;
             else {
                uint8_t dummy = 0;
                if (is_dbcast(dip, &dummy)) is_bcast = true;
            }

            if (is_bcast) {
                if (dip == 0xFFFFFFFFu) {
                    if (bound_l3_count == 0) return SOCK_ERR_SYS;

                    for (int i = 0; i < bound_l3_count; ++i) {
                        uint8_t bl3 = bound_l3[i];
                        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(bl3);
                        if (!is_valid_v4_l3_for_bind(v4)) continue;
                        if (!v4->l2) continue;

                        if (!bound) {
                            int p = udp_alloc_ephemeral_l3(bl3, pid, dispatch);
                            if (p < 0) continue;
                            localPort = (uint16_t)p;
                            add_bound_l3(bl3);
                            bound = true;
                        } else if (localPort == 0) {
                            int p = udp_alloc_ephemeral_l3(bl3, pid, dispatch);
                            if (p < 0) continue;
                            localPort = (uint16_t)p;
                        }

                        net_l4_endpoint src;
                        src.ver = IP_VER4;
                        memset(src.ip, 0, 16);
                        memcpy(src.ip, &v4->ip, 4);
                        src.port = localPort;

                        ipv4_tx_opts_t tx;
                        tx.scope = IP_TX_BOUND_L3;
                        tx.index = bl3;

                        udp_send_segment(&src, &d, pay, &tx, (extraOpts.flags & SOCK_OPT_TTL) ? extraOpts.ttl : 0, (extraOpts.flags & SOCK_OPT_DONTFRAG) ? 1 : 0);
                    }

                    remoteEP = d;
                    return (int64_t)len;
                }

                uint8_t db_l3 = 0;
                if (!is_dbcast(dip, &db_l3)) return SOCK_ERR_SYS;

                l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(db_l3);
                if (!is_valid_v4_l3_for_bind(v4)) return SOCK_ERR_SYS;
                if (!v4->l2) return SOCK_ERR_SYS;

                if (!bound) {
                    int p = udp_alloc_ephemeral_l3(db_l3, pid, dispatch);
                    if (p < 0) return SOCK_ERR_NO_PORT;
                    localPort = (uint16_t)p;
                    add_bound_l3(db_l3);
                    bound = true;
                } else if (localPort == 0) {
                    int p = udp_alloc_ephemeral_l3(db_l3, pid, dispatch);
                    if (p < 0) return SOCK_ERR_NO_PORT;
                    localPort = (uint16_t)p;
                }

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
                if (bound_l3_count == 0) return SOCK_ERR_SYS;

                for (int i = 0; i < bound_l3_count; ++i) {
                    uint8_t bl3 = bound_l3[i];
                    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(bl3);
                    if (!is_valid_v4_l3_for_bind(v4)) continue;
                    if (!v4->l2) continue;

                    if (!bound) {
                        int p = udp_alloc_ephemeral_l3(bl3, pid, dispatch);
                        if (p < 0) continue;
                        localPort = (uint16_t)p;
                        add_bound_l3(bl3);
                        bound = true;
                    } else if (localPort == 0) {
                        int p = udp_alloc_ephemeral_l3(bl3, pid, dispatch);
                        if (p < 0) continue;
                        localPort = (uint16_t)p;
                    }

                    (void)l2_ipv4_mcast_join(v4->l2->ifindex, dip);
                    (void)igmp_send_join(v4->l2->ifindex, dip);

                    net_l4_endpoint src;
                    src.ver = IP_VER4;
                    memset(src.ip, 0, 16);
                    memcpy(src.ip, &v4->ip, 4);
                    src.port = localPort;

                    ipv4_tx_opts_t tx;
                    tx.scope = IP_TX_BOUND_L3;
                    tx.index = bl3;

                    udp_send_segment(&src, &d, pay, &tx, (extraOpts.flags & SOCK_OPT_TTL) ? extraOpts.ttl : 0, (extraOpts.flags & SOCK_OPT_DONTFRAG) ? 1 : 0);
                }

                remoteEP = d;
                return (int64_t)len;
            }

            uint8_t allowed_v4[SOCK_MAX_L3];
            int n_allowed = 0;
            for (int i = 0; i < bound_l3_count && n_allowed < SOCK_MAX_L3; ++i) {
                uint8_t id = bound_l3[i];
                if (l3_ipv4_find_by_id(id)) allowed_v4[n_allowed++] = id;
            }
            if (bound_l3_count > 0 && n_allowed == 0) return SOCK_ERR_SYS;

            ipv4_tx_plan_t plan;
            if (!ipv4_build_tx_plan(dip, nullptr, n_allowed ? allowed_v4 : nullptr, n_allowed, &plan)) return SOCK_ERR_SYS;

            uint8_t chosen_l3 = plan.l3_id;
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
            if (!is_valid_v4_l3_for_bind(v4)) return SOCK_ERR_SYS;

            if (!bound) {
                int p = udp_alloc_ephemeral_l3(chosen_l3, pid, dispatch);
                if (p < 0) return SOCK_ERR_NO_PORT;
                localPort = (uint16_t)p;
                add_bound_l3(chosen_l3);
                bound = true;
            } else if (localPort == 0) {
                int p = udp_alloc_ephemeral_l3(chosen_l3, pid, dispatch);
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
                if (!bound) return SOCK_ERR_BOUND;
                if (!localPort) return SOCK_ERR_BOUND;
                if (bound_l3_count == 0) return SOCK_ERR_SYS;

                for (int i = 0; i < bound_l3_count; ++i) {
                    uint8_t bl3 = bound_l3[i];
                    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(bl3);
                    if (!is_valid_v6_l3_for_bind(v6)) continue;

                    net_l4_endpoint src;
                    src.ver = IP_VER6;
                    memset(src.ip, 0, 16);
                    memcpy(src.ip, v6->ip, 16);
                    src.port = localPort;

                    ipv6_tx_opts_t tx;
                    tx.scope = IP_TX_BOUND_L3;
                    tx.index = bl3;

                    udp_send_segment(&src, &d, pay, &tx, (extraOpts.flags & SOCK_OPT_TTL) ? extraOpts.ttl : 0, (extraOpts.flags & SOCK_OPT_DONTFRAG) ? 1 : 0);
                }

                remoteEP = d;
                return (int64_t)len;
            }

            uint8_t allowed_v6[SOCK_MAX_L3];
            int n_allowed = 0;
            for (int i = 0; i < bound_l3_count && n_allowed < SOCK_MAX_L3; ++i) {
                uint8_t id = bound_l3[i];
                if (l3_ipv6_find_by_id(id)) allowed_v6[n_allowed++] = id;
            }
            if (bound_l3_count > 0 && n_allowed == 0) return SOCK_ERR_SYS;

            ipv6_tx_plan_t plan;
            if (!ipv6_build_tx_plan(d.ip, nullptr, n_allowed ? allowed_v6 : nullptr, n_allowed, &plan)) return SOCK_ERR_SYS;

            uint8_t chosen_l3 = plan.l3_id;

            l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(chosen_l3);
            if (!is_valid_v6_l3_for_bind(v6)) return SOCK_ERR_SYS;

            if (!bound) {
                int p = udp_alloc_ephemeral_l3(chosen_l3, pid, dispatch);
                if (p < 0) return SOCK_ERR_NO_PORT;
                localPort = (uint16_t)p;
                add_bound_l3(chosen_l3);
                bound = true;
            } else if (localPort == 0) {
                int p = udp_alloc_ephemeral_l3(chosen_l3, pid, dispatch);
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

        free_sized((void*)p.ptr, p.size);
        remoteEP = se;
        return tocpy;
    }

    int32_t close() override {
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_UDP;
        ev.action = NETLOG_ACT_CLOSE;
        ev.pid = pid;
        ev.local_port = localPort;
        ev.remote_ep = remoteEP;
        netlog_socket_event(&extraOpts, &ev);
        while (r_head != r_tail) {
            rx_bytes -= ring[r_head].size;
            free_sized((void*)ring[r_head].ptr, ring[r_head].size);
            r_head = (r_head + 1) % UDP_RING_CAP;
        }
        return Socket::close();
    }

    net_l4_endpoint get_remote_ep() const {
        return remoteEP;
    }
};