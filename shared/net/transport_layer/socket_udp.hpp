#pragma once
#include "socket.hpp"
#include "net/transport_layer/udp.h"
#include "types.h"
#include "std/memory.h"
#include "net/application_layer/dns.h"
#include "net/transport_layer/socket_types.h"
#include "net/internet_layer/ipv4_route.h"
#include "net/internet_layer/ipv6_route.h"
#include "syscalls/syscalls.h"
#include "net/net.h"
#include "net/internet_layer/ipv4_utils.h"
#include "net/internet_layer/ipv6_utils.h"
#include "net/internet_layer/ipv6.h"

static constexpr int32_t UDP_RING_CAP = 1024;
static constexpr dns_server_sel_t UDP_DNS_SEL = DNS_USE_BOTH;
static constexpr uint32_t UDP_DNS_TIMEOUT_MS = 3000;

class UDPSocket : public Socket {
    inline static UDPSocket* s_list_head = nullptr;

    sizedptr ring[UDP_RING_CAP];
    net_l4_endpoint src_eps[UDP_RING_CAP];
    int32_t r_head = 0;
    int32_t r_tail = 0;

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

            for (int i = 0; i < s->bound_l3_count; ++i) {
                uint8_t id = s->bound_l3[i];
                l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(id);
                if (!is_valid_v4_l3_for_bind(v4)) continue;
                if (v4->l2->ifindex != ifx) continue;
                if (lb) return true;

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

    static void dispatch(uint8_t ifindex, ip_version_t ipver, const void* src_ip_addr, const void* dst_ip_addr, uintptr_t frame_ptr, uint32_t frame_len, uint16_t src_port, uint16_t dst_port) {
        for (UDPSocket* s = s_list_head; s; s = s->next) {
            if (!socket_matches_dst(s, ifindex, ipver, dst_ip_addr, dst_port)) continue;
            s->on_receive(ipver, src_ip_addr, src_port, frame_ptr, frame_len);
            return;
        }
        if (frame_ptr && frame_len) free((void*)frame_ptr, frame_len);
    }

    void on_receive(ip_version_t ver, const void* src_ip_addr, uint16_t src_port, uintptr_t ptr, uint32_t len) {
        uintptr_t copy = (uintptr_t)malloc(len);
        if (!copy) {
            if (ptr && len) free((void*)ptr, len);
            return;
        }

        memcpy((void*)copy, (void*)ptr, len);
        if (ptr && len) free((void*)ptr, len);

        int nexti = (r_tail + 1) % UDP_RING_CAP;
        if (nexti == r_head) {
            free((void*)ring[r_head].ptr, ring[r_head].size);
            r_head = (r_head + 1) % UDP_RING_CAP;
        }

        ring[r_tail].ptr = copy;
        ring[r_tail].size = len;

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
        if (n <= 0) return false;
        if (!ipv4_rt_pick_best_l3_in(candidates, n, dip, out_l3)) return false;
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(*out_l3);
        if (!is_valid_v4_l3_for_bind(v4)) return false;
        return true;
    }

    static bool pick_v6_l3_for_unicast(const uint8_t dst_ip[16], const uint8_t* candidates, int n, uint8_t* out_l3) {
        if (!out_l3) return false;

        ip_resolution_result_t r = resolve_ipv6_to_interface(dst_ip);
        if (!r.found) return false;
        if (!r.ipv6) return false;

        if (n <= 0) {
            *out_l3 = r.ipv6->l3_id;
            return true;
        }

        for (int i = 0; i < n; ++i) {
            if (candidates[i] == r.ipv6->l3_id) {
                *out_l3 = r.ipv6->l3_id;
                return true;
            }
        }

        return false;
    }

public:
    UDPSocket(uint8_t r, uint32_t pid_) : Socket(PROTO_UDP, r) {
        pid = pid_;
        insert_in_list();
    }

    ~UDPSocket() override {
        close();
        remove_from_list();
    }

    int32_t bind(const SockBindSpec& spec, uint16_t port) override {
        if (role != SOCK_ROLE_SERVER) return SOCK_ERR_PERM;
        if (bound) return SOCK_ERR_BOUND;

        uint8_t ids[SOCK_MAX_L3];
        int n = 0;

        if (spec.kind == BIND_L3) {
            if (spec.l3_id) ids[n++] = spec.l3_id;
            if (n == 0) return SOCK_ERR_INVAL;
        } else if (spec.kind == BIND_L2) {
            if (!add_all_l3_on_l2(spec.ifindex, ids, n)) return SOCK_ERR_INVAL;
        } else if (spec.kind == BIND_IP) {
            if (spec.ver == IP_VER4) {
                uint32_t v4ip = 0;
                memcpy(&v4ip, spec.ip, 4);
                l3_ipv4_interface_t* ipif = l3_ipv4_find_by_ip(v4ip);
                if (!is_valid_v4_l3_for_bind(ipif)) return SOCK_ERR_INVAL;
                ids[n++] = ipif->l3_id;
            } else if (spec.ver == IP_VER6) {
                l3_ipv6_interface_t* ipif6 = l3_ipv6_find_by_ip(spec.ip);
                if (!is_valid_v6_l3_for_bind(ipif6)) return SOCK_ERR_INVAL;
                ids[n++] = ipif6->l3_id;
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
            if (!seen) dedup[m++] = ids[i];
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

        localPort = port;
        bound = true;
        return SOCK_OK;
    }

    int64_t sendto(SockDstKind kind, const void* dst, uint16_t port, const void* buf, uint64_t len) {
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
            uint32_t a4 = 0;
            dns_result_t dr = dns_resolve_a(host, &a4, UDP_DNS_SEL, UDP_DNS_TIMEOUT_MS);
            if (dr != DNS_OK) return SOCK_ERR_DNS;
            d.ver = IP_VER4;
            memset(d.ip, 0, 16);
            memcpy(d.ip, &a4, 4);
            d.port = port;
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

                        udp_send_segment(&src, &d, pay, &tx);
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

                udp_send_segment(&src, &d, pay, &tx);
                remoteEP = d;
                return (int64_t)len;
            }

            if (ipv4_is_multicast(dip)) return SOCK_ERR_PROTO;

            uint8_t chosen_l3 = 0;

            if (bound_l3_count == 0) {
                uint8_t ids[SOCK_MAX_L3];
                int n = 0;

                uint8_t cnt = l2_interface_count();
                for (uint8_t i = 0; i < cnt && n < SOCK_MAX_L3; ++i) {
                    l2_interface_t* l2 = l2_interface_at(i);
                    if (!l2) continue;
                    if (!l2->is_up) continue;

                    for (int s = 0; s < MAX_IPV4_PER_INTERFACE && n < SOCK_MAX_L3; ++s) {
                        l3_ipv4_interface_t* v4 = l2->l3_v4[s];
                        if (!is_valid_v4_l3_for_bind(v4)) continue;
                        ids[n++] = v4->l3_id;
                    }
                }

                if (!pick_v4_l3_for_unicast(dip, ids, n, &chosen_l3)) return SOCK_ERR_SYS;

                l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
                if (!is_valid_v4_l3_for_bind(v4)) return SOCK_ERR_SYS;

                int p = udp_alloc_ephemeral_l3(chosen_l3, pid, dispatch);
                if (p < 0) return SOCK_ERR_NO_PORT;

                localPort = (uint16_t)p;
                add_bound_l3(chosen_l3);
                bound = true;

                net_l4_endpoint src;
                src.ver = IP_VER4;
                memset(src.ip, 0, 16);
                memcpy(src.ip, &v4->ip, 4);
                src.port = localPort;

                udp_send_segment(&src, &d, pay, nullptr);
                remoteEP = d;
                return (int64_t)len;
            }

            if (bound_l3_count == 1) {
                chosen_l3 = bound_l3[0];

                l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
                if (!is_valid_v4_l3_for_bind(v4)) return SOCK_ERR_SYS;

                if (localPort == 0) {
                    int p = udp_alloc_ephemeral_l3(chosen_l3, pid, dispatch);
                    if (p < 0) return SOCK_ERR_NO_PORT;
                    localPort = (uint16_t)p;
                }

                net_l4_endpoint src;
                src.ver = IP_VER4;
                memset(src.ip, 0, 16);
                memcpy(src.ip, &v4->ip, 4);
                src.port = localPort;

                udp_send_segment(&src, &d, pay, nullptr);
                remoteEP = d;
                return (int64_t)len;
            }

            uint8_t ids[SOCK_MAX_L3];
            int n = 0;
            for (int i = 0; i < bound_l3_count && n < SOCK_MAX_L3; ++i) ids[n++] = bound_l3[i];

            if (!pick_v4_l3_for_unicast(dip, ids, n, &chosen_l3)) return SOCK_ERR_SYS;

            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
            if (!is_valid_v4_l3_for_bind(v4)) return SOCK_ERR_SYS;

            if (localPort == 0) {
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
            tx.scope = IP_TX_BOUND_L3;
            tx.index = chosen_l3;

            udp_send_segment(&src, &d, pay, &tx);
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

                    udp_send_segment(&src, &d, pay, &tx);
                }

                remoteEP = d;
                return (int64_t)len;
            }


            uint8_t chosen_l3 = 0;

            if (bound_l3_count == 0) {
                if (!pick_v6_l3_for_unicast(d.ip, nullptr, 0, &chosen_l3)) return SOCK_ERR_SYS;
            } else if (bound_l3_count == 1) {
                uint8_t cand = bound_l3[0];
                l3_ipv6_interface_t* v6c = l3_ipv6_find_by_id(cand);
                if (!is_valid_v6_l3_for_bind(v6c)) return SOCK_ERR_SYS;
                chosen_l3 = cand;
            } else {
                uint8_t ids[SOCK_MAX_L3];
                int n = 0;
                for (int i = 0; i < bound_l3_count && n < SOCK_MAX_L3; ++i) ids[n++] = bound_l3[i];
                if (!pick_v6_l3_for_unicast(d.ip, ids, n, &chosen_l3)) return SOCK_ERR_SYS;
            }

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
            tx.scope = IP_TX_BOUND_L3;
            tx.index = chosen_l3;

            udp_send_segment(&src, &d, pay, &tx);
            remoteEP = d;
            return (int64_t)len;
        }

        return SOCK_ERR_INVAL;
    }

    int64_t recvfrom(void* buf, uint64_t len, net_l4_endpoint* src) {
        if (r_head == r_tail) return 0;

        sizedptr p = ring[r_head];
        net_l4_endpoint se = src_eps[r_head];
        r_head = (r_head + 1) % UDP_RING_CAP;

        uint32_t tocpy = p.size;
        if (tocpy > len) tocpy = (uint32_t)len;

        memcpy(buf, (void*)p.ptr, tocpy);
        if (src) *src = se;

        free((void*)p.ptr, p.size);
        remoteEP = se;
        return tocpy;
    }

    int32_t close() override {
        while (r_head != r_tail) {
            free((void*)ring[r_head].ptr, ring[r_head].size);
            r_head = (r_head + 1) % UDP_RING_CAP;
        }
        return Socket::close();
    }

    net_l4_endpoint get_remote_ep() const {
        return remoteEP;
    }
};
