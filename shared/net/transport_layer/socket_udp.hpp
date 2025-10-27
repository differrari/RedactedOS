#pragma once
#include "socket.hpp"
#include "net/transport_layer/udp.h"
#include "types.h"
#include "std/memory.h"
#include "net/internet_layer/ipv4.h"
#include "net/application_layer/dns.h"
#include "net/transport_layer/socket_types.h"
#include "net/internet_layer/ipv4_route.h"
#include "syscalls/syscalls.h"
#include "net/net.h"
#include "net/internet_layer/ipv4_utils.h"

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

    static bool is_lbcast_ip(uint32_t ip) {
        return ip == 0xFFFFFFFFu;
    }

    static bool is_dbcast_for(const l3_ipv4_interface_t* v4, uint32_t dst) {
        if (!v4 || !v4->mask) return false;
        uint32_t b = ipv4_broadcast_calc(v4->ip, v4->mask);
        return b == dst;
    }

    static bool socket_matches_dst(UDPSocket* s, uint8_t ifx, ip_version_t ver, const void* dst_ip_addr, uint16_t dst_port) {
        if (!s->bound) return false;
        if (s->localPort != dst_port) return false;
        if (ver == IP_VER4) {
            uint32_t dip = *(const uint32_t*)dst_ip_addr;
            bool lb = is_lbcast_ip(dip);
            for (int i = 0; i < s->bound_l3_count; ++i) {
                uint8_t id = s->bound_l3[i];
                l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(id);
                if (!v4 || !v4->l2) continue;
                if (v4->l2->ifindex != ifx) continue;
                if (lb) return true;
                if (is_dbcast_for(v4, dip)) return true;
                if (v4->ip && v4->ip == dip) return true;
            }
            return false;
        } else if (ver == IP_VER6) {
            for (int i = 0; i < s->bound_l3_count; ++i) {
                uint8_t id = s->bound_l3[i];
                l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(id);
                if (!v6 || !v6->l2) continue;
                if (v6->l2->ifindex != ifx) continue;
                if (memcmp(v6->ip, dst_ip_addr, 16) == 0) return true;
            }
            return false;
        } else {
            return false;
        }
    }

    static void dispatch(uint8_t ifindex, ip_version_t ipver, const void* src_ip_addr, const void* dst_ip_addr, uintptr_t frame_ptr, uint32_t frame_len, uint16_t src_port, uint16_t dst_port) {
        for (UDPSocket* s = s_list_head; s; s = s->next) {
            if (socket_matches_dst(s, ifindex, ipver, dst_ip_addr, dst_port)) {
                s->on_receive(ipver, src_ip_addr, src_port, frame_ptr, frame_len);
                return;
            }
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

        ring[r_tail] = { (uintptr_t)copy, len };
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
        for (int s = 0; s < MAX_IPV4_PER_INTERFACE; ++s) {
            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
            if (v4 && v4->mode != IPV4_CFG_DISABLED) {
                if (n < SOCK_MAX_L3) tmp_ids[n++] = v4->l3_id;
            }
        }
        for (int s = 0; s < MAX_IPV6_PER_INTERFACE; ++s) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[s];
            if (v6 && v6->cfg != IPV6_CFG_DISABLE) {
                if (n < SOCK_MAX_L3) tmp_ids[n++] = v6->l3_id;
            }
        }
        return n > 0;
    }

    bool add_all_l3_any(uint8_t* tmp_ids, int& n) {
        uint8_t cnt = l2_interface_count();
        for (uint8_t i = 0; i < cnt; ++i) {
            l2_interface_t* l2 = l2_interface_at(i);
            if (!l2) continue;
            for (int s = 0; s < MAX_IPV4_PER_INTERFACE; ++s) {
                l3_ipv4_interface_t* v4 = l2->l3_v4[s];
                if (v4 && v4->mode != IPV4_CFG_DISABLED) {
                    if (n < SOCK_MAX_L3) tmp_ids[n++] = v4->l3_id;
                }
            }
            for (int s = 0; s < MAX_IPV6_PER_INTERFACE; ++s) {
                l3_ipv6_interface_t* v6 = l2->l3_v6[s];
                if (v6 && v6->cfg != IPV6_CFG_DISABLE) {
                    if (n < SOCK_MAX_L3) tmp_ids[n++] = v6->l3_id;
                }
            }
        }
        return n > 0;
    }

    void do_unbind_one(uint8_t l3_id, uint16_t port, uint16_t pid_) override {
        (void)pid_;
        udp_unbind_l3(l3_id, port, pid);
    }

    static bool contains_id(const uint8_t* arr, int n, uint8_t id) {
        for (int i = 0; i < n; ++i) if (arr[i] == id) return true;
        return false;
    }

    static bool is_lbcast(uint32_t ip) {
        return ip == 0xFFFFFFFFu;
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
                if (!v4->ip || !v4->mask) continue;
                uint32_t b = ipv4_broadcast_calc(v4->ip, v4->mask);
                if (b == ip) { if (out_l3) *out_l3 = v4->l3_id; return true; }
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
            if (spec.l3_id) { ids[n++] = spec.l3_id; }
        } else if (spec.kind == BIND_L2) {
            if (!add_all_l3_on_l2(spec.ifindex, ids, n)) return SOCK_ERR_INVAL;
        } else if (spec.kind == BIND_IP) {
            if (spec.ver == IP_VER4) {
                uint32_t v4;
                memcpy(&v4, spec.ip, 4);
                v4 = __builtin_bswap32(v4);
                ip_resolution_result_t r = resolve_ipv4_to_interface(v4);
                if (!r.found || !r.ipv4) return SOCK_ERR_INVAL;
                ids[n++] = r.ipv4->l3_id;
            } else if (spec.ver == IP_VER6) {
                ip_resolution_result_t r6 = resolve_ipv6_to_interface(spec.ip);
                if (!r6.found || !r6.ipv6) return SOCK_ERR_INVAL;
                ids[n++] = r6.ipv6->l3_id;
            } else {
                return SOCK_ERR_INVAL;
            }
        } else if (spec.kind == BIND_ANY) {
            if (!add_all_l3_any(ids, n)) return SOCK_ERR_INVAL;
        } else {
            return SOCK_ERR_INVAL;
        }
        uint8_t dedup[SOCK_MAX_L3];
        int m = 0;
        for (int i = 0; i < n; ++i) { if (!contains_id(dedup, m, ids[i])) dedup[m++] = ids[i]; }
        int bdone = 0;
        for (int i = 0; i < m; ++i) {
            uint8_t id = dedup[i];
            bool ok = udp_bind_l3(id, port, pid, dispatch);
            if (!ok) {
                for (int j = 0; j < bdone; ++j) {
                    uint8_t rid = dedup[j];
                    udp_unbind_l3(rid, port, pid);
                }
                return SOCK_ERR_SYS;
            }
            bdone++;
        }
        clear_bound_l3();
        for (int i = 0; i < m; ++i) add_bound_l3(dedup[i]);
        localPort = port;
        bound = true;
        return SOCK_OK;
    }

    int64_t sendto(SockDstKind kind, const void* dst, uint16_t port, const void* buf, uint64_t len) {
        if (!dst || !buf || len == 0) return SOCK_ERR_INVAL;
        net_l4_endpoint d{};
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

        if (d.ver == IP_VER4) {
            uint32_t dip; memcpy(&dip, d.ip, 4);
            uint8_t chosen_l3 = 0;
            bool is_bcast = false;
            uint8_t db_l3 = 0;
            if (is_lbcast(dip)) {
                is_bcast = true;
            } else if (is_dbcast(dip, &db_l3)) {
                is_bcast = true;
                chosen_l3 = db_l3;
            }

            if (is_bcast) {
                if (dip == 0xFFFFFFFFu) {
                    if (bound_l3_count == 0) return SOCK_ERR_SYS;
                    for (int i = 0; i < bound_l3_count; ++i) {
                        uint8_t bl3 = bound_l3[i];
                        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(bl3);
                        if (!v4 || !v4->l2) continue;
                        if (!bound) {
                            int p = udp_alloc_ephemeral_l3(bl3, pid, dispatch);
                            if (p < 0) continue;
                            localPort = (uint16_t)p;
                            add_bound_l3(bl3);
                            bound = true;
                        } else {
                            bool present = false;
                            for (int k = 0; k < bound_l3_count; ++k) if (bound_l3[k] == bl3) { present = true; break; }
                            if (!present) {
                                if (!udp_bind_l3(bl3, localPort, pid, dispatch)) continue;
                                add_bound_l3(bl3);
                            }
                        }
                        net_l4_endpoint src;
                        src.ver = IP_VER4;
                        memset(src.ip, 0, 16);
                        memcpy(src.ip, &v4->ip, 4);
                        src.port = localPort;
                        sizedptr pay{ (uintptr_t)buf, (uint32_t)len };
                        ipv4_tx_opts_t tx; tx.scope = IP_TX_BOUND_L3; tx.index = bl3;
                        udp_send_segment(&src, &d, pay, &tx);
                    }
                    remoteEP = d;
                    return (int64_t)len;
                } else {
                    if (!chosen_l3) return SOCK_ERR_SYS;
                    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
                    if (!v4 || !v4->l2) return SOCK_ERR_SYS;
                    if (!bound) {
                        int p = udp_alloc_ephemeral_l3(chosen_l3, pid, dispatch);
                        if (p < 0) return SOCK_ERR_NO_PORT;
                        localPort = (uint16_t)p;
                        add_bound_l3(chosen_l3);
                        bound = true;
                    } else {
                        bool present = false;
                        for (int i = 0; i < bound_l3_count; ++i) if (bound_l3[i] == chosen_l3) { present = true; break; }
                        if (!present) {
                            if (!udp_bind_l3(chosen_l3, localPort, pid, dispatch)) return SOCK_ERR_SYS;
                            add_bound_l3(chosen_l3);
                        }
                    }
                    net_l4_endpoint src;
                    src.ver = IP_VER4;
                    memset(src.ip, 0, 16);
                    memcpy(src.ip, &v4->ip, 4);
                    src.port = localPort;
                    sizedptr pay{ (uintptr_t)buf, (uint32_t)len };
                    ipv4_tx_opts_t tx; tx.scope = IP_TX_BOUND_L3; tx.index = chosen_l3;
                    udp_send_segment(&src, &d, pay, &tx);
                    remoteEP = d;
                    return (int64_t)len;
                }
            } else {
                if (bound_l3_count == 0) {
                    uint8_t ids[SOCK_MAX_L3]; int n = 0;
                    uint8_t cnt = l2_interface_count();
                    for (uint8_t i = 0; i < cnt; ++i) {
                        l2_interface_t* l2 = l2_interface_at(i);
                        if (!l2 || !l2->is_up) continue;
                        for (int s = 0; s < MAX_IPV4_PER_INTERFACE && n < SOCK_MAX_L3; ++s) {
                            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
                            if (!v4) continue;
                            if (v4->mode == IPV4_CFG_DISABLED) continue;
                            ids[n++] = v4->l3_id;
                        }
                    }
                    if (n == 0) return SOCK_ERR_SYS;
                    if (!ipv4_rt_pick_best_l3_in(ids, n, dip, &chosen_l3)) return SOCK_ERR_SYS;
                    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
                    if (!v4 || !v4->l2) return SOCK_ERR_SYS;
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
                    sizedptr pay{ (uintptr_t)buf, (uint32_t)len };
                    const ipv4_tx_opts_t* txp = nullptr;
                    udp_send_segment(&src, &d, pay, txp);
                    remoteEP = d;
                    return (int64_t)len;
                } else if (bound_l3_count == 1) {
                    chosen_l3 = bound_l3[0];
                    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
                    if (!v4 || !v4->l2) return SOCK_ERR_SYS;
                    net_l4_endpoint src;
                    src.ver = IP_VER4;
                    memset(src.ip, 0, 16);
                    memcpy(src.ip, &v4->ip, 4);
                    src.port = localPort;
                    sizedptr pay{ (uintptr_t)buf, (uint32_t)len };
                    const ipv4_tx_opts_t* txp = nullptr;
                    udp_send_segment(&src, &d, pay, txp);
                    remoteEP = d;
                    return (int64_t)len;
                } else {
                    if (!ipv4_rt_pick_best_l3_in(bound_l3, bound_l3_count, dip, &chosen_l3)) return SOCK_ERR_SYS;
                    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
                    if (!v4 || !v4->l2) return SOCK_ERR_SYS;
                    bool present = false;
                    for (int i = 0; i < bound_l3_count; ++i) if (bound_l3[i] == chosen_l3) { present = true; break; }
                    if (!present) {
                        if (!udp_bind_l3(chosen_l3, localPort, pid, dispatch)) return SOCK_ERR_SYS;
                        add_bound_l3(chosen_l3);
                    }
                    net_l4_endpoint src;
                    src.ver = IP_VER4;
                    memset(src.ip, 0, 16);
                    memcpy(src.ip, &v4->ip, 4);
                    src.port = localPort;
                    sizedptr pay{ (uintptr_t)buf, (uint32_t)len };
                    ipv4_tx_opts_t tx; tx.scope = IP_TX_BOUND_L3; tx.index = chosen_l3;
                    udp_send_segment(&src, &d, pay, &tx);
                    remoteEP = d;
                    return (int64_t)len;
                }
            }
        } else if (d.ver == IP_VER6) {
            return SOCK_ERR_PROTO;
        } else {
            return SOCK_ERR_INVAL;
        }
    }

    int64_t recvfrom(void* buf, uint64_t len, net_l4_endpoint* src) {
        if (r_head == r_tail) return 0;
        sizedptr p = ring[r_head];
        net_l4_endpoint se = src_eps[r_head];
        r_head = (r_head + 1) % UDP_RING_CAP;
        uint32_t tocpy = p.size < len ? p.size : (uint32_t)len;
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
