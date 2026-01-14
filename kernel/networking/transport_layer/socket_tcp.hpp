#pragma once

#include "std/memory.h"
#include "std/string.h"
#include "socket.hpp"
#include "networking/transport_layer/tcp.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/application_layer/dns/dns.h"
#include "types.h"
#include "data_struct/ring_buffer.hpp"
#include "net/socket_types.h"
#include "networking/internet_layer/ipv4_route.h"
#include "networking/internet_layer/ipv6_route.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/transport_layer/trans_utils.h"
#include "syscalls/syscalls.h"

static constexpr int TCP_MAX_BACKLOG = 8;
static constexpr dns_server_sel_t TCP_DNS_SEL = DNS_USE_BOTH;
static constexpr uint32_t TCP_DNS_TIMEOUT_MS = 3000;

class TCPSocket : public Socket {
    inline static TCPSocket* s_list_head = nullptr;

    static constexpr uint32_t TCP_RING_CAP = 256 * 1024;
    RingBuffer<uint8_t, TCP_RING_CAP> ring;
    tcp_data* flow = nullptr;

    TCPSocket* pending[TCP_MAX_BACKLOG] = { nullptr };
    int backlogCap = 0;
    int backlogLen = 0;
    TCPSocket* next = nullptr;

    static bool is_valid_v4_l3_for_bind(l3_ipv4_interface_t* v4) {
        if (!v4 || !v4->l2) return false;
        if (!v4->l2->is_up) return false;
        if (v4->is_localhost) return false;
        if (v4->mode == IPV4_CFG_DISABLED) return false;
        if (v4->ip == 0) return false;
        if (!v4->port_manager) return false;
        return true;
    }

    static bool is_valid_v6_l3_for_bind(l3_ipv6_interface_t* v6) {
        if (!v6 || !v6->l2) return false;
        if (!v6->l2->is_up) return false;
        if (v6->is_localhost) return false;
        if (v6->cfg == IPV6_CFG_DISABLE) return false;
        if (ipv6_is_unspecified(v6->ip)) return false;
        if (v6->dad_state == IPV6_DAD_FAILED) return false;
        if (!(v6->kind & IPV6_ADDRK_LINK_LOCAL) && v6->dad_state != IPV6_DAD_OK) return false;
        if (!v6->port_manager) return false;
        return true;
    }

    static uint32_t dispatch(uint8_t ifindex, ip_version_t ipver, const void* src_ip_addr, const void* dst_ip_addr, uintptr_t frame_ptr, uint32_t frame_len, uint16_t src_port, uint16_t dst_port) {
        if (frame_len == 0){
            for (TCPSocket* srv = s_list_head; srv; srv = srv->next){
                if (srv->role != SOCK_ROLE_SERVER) continue;
                if (!srv->bound) continue;
                if (srv->localPort != dst_port) continue;

                bool matches_dst = false;
                for (int i = 0; i < srv->bound_l3_count; ++i) {
                    uint8_t id = srv->bound_l3[i];

                    if (ipver == IP_VER4) {
                        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(id);
                        if (!is_valid_v4_l3_for_bind(v4)) continue;
                        if (v4->l2->ifindex != ifindex) continue;
                        if (v4->ip == *(const uint32_t*)dst_ip_addr) {
                            matches_dst = true;
                            break;
                        }
                    } else if (ipver == IP_VER6) {
                        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(id);
                        if (!is_valid_v6_l3_for_bind(v6)) continue;
                        if (v6->l2->ifindex != ifindex) continue;
                        if (memcmp(v6->ip, dst_ip_addr, 16) == 0){
                            matches_dst = true;
                            break;
                        }
                    }
                }

                if (!matches_dst) continue;
                if (srv->backlogLen >= srv->backlogCap) break;

                TCPSocket* child = new TCPSocket(SOCK_ROLE_CLIENT, srv->pid, &srv->extraOpts);

                child->localPort = dst_port;
                child->connected = true;

                child->remoteEP.ver = ipver;
                memset(child->remoteEP.ip, 0, 16);
                if (ipver == IP_VER4) memcpy(child->remoteEP.ip, src_ip_addr, 4);
                else memcpy(child->remoteEP.ip, src_ip_addr, 16);
                child->remoteEP.port = src_port;

                uint8_t l3id = 0;

                for (int i = 0; i < srv->bound_l3_count; ++i) {
                    uint8_t id = srv->bound_l3[i];

                    if (ipver == IP_VER4) {
                        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(id);
                        if (!is_valid_v4_l3_for_bind(v4)) continue;
                        if (v4->l2->ifindex != ifindex) continue;
                        if (v4->ip == *(const uint32_t*)dst_ip_addr) {
                            l3id = id;
                            break;
                        }
                    } else if (ipver == IP_VER6) {
                        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(id);
                        if (!is_valid_v6_l3_for_bind(v6)) continue;
                        if (v6->l2->ifindex != ifindex) continue;
                        if (memcmp(v6->ip, dst_ip_addr, 16) == 0) {
                            l3id = id;
                            break;
                        }
                    }
                }

                if (!l3id) {
                    if (ipver == IP_VER4) {
                        uint32_t v4dst = 0;
                        memcpy(&v4dst, dst_ip_addr, 4);
                        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_ip(v4dst);
                        if (is_valid_v4_l3_for_bind(v4) && v4->l2 && v4->l2->ifindex == ifindex) l3id = v4->l3_id;
                    } else if (ipver == IP_VER6) {
                        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_ip((const uint8_t*)dst_ip_addr);
                        if (is_valid_v6_l3_for_bind(v6) && v6->l2 && v6->l2->ifindex == ifindex) l3id = v6->l3_id;
                    }
                }

                child->clear_bound_l3();

                if (l3id) {
                    child->add_bound_l3(l3id);
                } else if (ipver == IP_VER4){
                    uint32_t v4dst = 0;
                    memcpy(&v4dst, dst_ip_addr, 4);
                    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_ip(v4dst);
                    if (is_valid_v4_l3_for_bind(v4) && v4->l2 && v4->l2->ifindex == ifindex) {
                        child->add_bound_l3(v4->l3_id);
                    } else {
                        for (int i = 0; i < srv->bound_l3_count; ++i) {
                            l3_ipv4_interface_t* sv4 = l3_ipv4_find_by_id(srv->bound_l3[i]);
                            if (!sv4 || !sv4->l2) continue;
                            if (!sv4->l2->is_up) continue;
                            if (sv4->l2->ifindex != ifindex) continue;
                            child->add_bound_l3(sv4->l3_id);
                        }
                    }
                } else if (ipver == IP_VER6) {
                    for (int i = 0; i < srv->bound_l3_count; ++i) {
                        l3_ipv6_interface_t* sv6 = l3_ipv6_find_by_id(srv->bound_l3[i]);
                        if (!is_valid_v6_l3_for_bind(sv6)) continue;
                        if (!sv6->l2 || !sv6->l2->is_up) continue;
                        if (sv6->l2->ifindex != ifindex) continue;
                        child->add_bound_l3(sv6->l3_id);
                    }
                }

                child->flow = tcp_get_ctx(dst_port, ipver, dst_ip_addr, child->remoteEP.ip, src_port);
                if (!child->flow){
                    child->close();
                    delete child;
                    break;
                }

			    child->insert_in_list();

                srv->pending[srv->backlogLen++] = child;
                break;
            }
            return 0;
        }

        for (TCPSocket* s = s_list_head; s; s = s->next) {
            if (!s->connected) continue;
            if (s->localPort != dst_port) continue;
            if (s->remoteEP.port != src_port) continue;
            if (s->remoteEP.ver != ipver) continue;

            bool matches_dst = (s->bound_l3_count == 0);
            for (int i = 0; !matches_dst && i < s->bound_l3_count; ++i) {
                uint8_t id = s->bound_l3[i];

                if (ipver == IP_VER4) {
                    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(id);
                    if (!is_valid_v4_l3_for_bind(v4)) continue;
                    if (v4->l2->ifindex != ifindex) continue;
                    if (v4->ip == *(const uint32_t*)dst_ip_addr) {
                        matches_dst = true;
                        break;
                    }
                } else {
                    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(id);
                    if (!is_valid_v6_l3_for_bind(v6)) continue;
                    if (v6->l2->ifindex != ifindex) continue;
                    if (memcmp(v6->ip, dst_ip_addr, 16) == 0) {
                        matches_dst = true;
                        break;
                    }
                }
            }

            if (!matches_dst) continue;

            if (ipver == IP_VER4) {
                if (*(const uint32_t*)s->remoteEP.ip != *(const uint32_t*)src_ip_addr) continue;
            } else {
                if (memcmp(s->remoteEP.ip, src_ip_addr, 16) != 0) continue;
            }

            return s->on_receive(frame_ptr, frame_len);
        }

        return 0;
    }
    
    uint32_t on_receive(uintptr_t ptr, uint32_t len) {
        if(!ptr || !len) return 0;

        uint64_t limit = ring.capacity();
        if ((extraOpts.flags & SOCK_OPT_BUF_SIZE) && extraOpts.buf_size) {
            uint64_t m = extraOpts.buf_size;
            if (m < limit) limit = m;
        }
        if (!limit) return 0;

        const uint8_t* src = (const uint8_t*)ptr;
        uint32_t pushed = 0;

        uint64_t sz = ring.size();
        if (sz < limit) {
            uint64_t free = limit - sz;

            uint32_t accept = len;
            if((uint64_t)accept > free) accept = (uint32_t)free;

            pushed = (uint32_t)ring.push_buf(src, accept);
        }

        return pushed;
    }

    void insert_in_list() {
        for (TCPSocket* it = s_list_head; it; it = it->next){
            if (it == this) {
                return;
            }
        }
        next = s_list_head;
        s_list_head = this;
    }

    void remove_from_list() {
        TCPSocket** cur = &s_list_head;
        while (*cur) {
            if (*cur == this) {
                *cur = (*cur)->next;
                break;
            }
            cur = &((*cur)->next);
        }
        next = nullptr;
    }

    void do_unbind_one(uint8_t l3_id, uint16_t port, uint16_t pid_) override {
        (void)pid_;
        if (role != SOCK_ROLE_SERVER) return;
        (void)tcp_unbind_l3(l3_id, port, pid);
    }

    bool add_all_l3_on_l2(uint8_t ifindex, uint8_t* tmp_ids, ip_version_t* tmp_ver, int& n) {
        l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
        if (!l2 || !l2->is_up) return false;

        for (int s = 0; s < MAX_IPV4_PER_INTERFACE; ++s) {
            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
            if (!is_valid_v4_l3_for_bind(v4)) continue;
            if (n < SOCK_MAX_L3) {
                tmp_ids[n] = v4->l3_id;
                tmp_ver[n] = IP_VER4;
                ++n;
            }
        }

        for (int s = 0; s < MAX_IPV6_PER_INTERFACE; ++s) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[s];
            if (!is_valid_v6_l3_for_bind(v6)) continue;
            if (n < SOCK_MAX_L3) {
                tmp_ids[n] = v6->l3_id;
                tmp_ver[n] = IP_VER6;
                ++n;
            }
        }

        return n > 0;
    }

    bool add_all_l3_any(uint8_t* tmp_ids, ip_version_t* tmp_ver, int& n) {
        uint8_t cnt = l2_interface_count();

        for (uint8_t i = 0; i < cnt; ++i) {
            l2_interface_t* l2 = l2_interface_at(i);
            if (!l2 || !l2->is_up) continue;

            for (int s = 0; s < MAX_IPV4_PER_INTERFACE; ++s) {
                l3_ipv4_interface_t* v4 = l2->l3_v4[s];
                if (!is_valid_v4_l3_for_bind(v4)) continue;
                if (n < SOCK_MAX_L3) {
                    tmp_ids[n] = v4->l3_id;
                    tmp_ver[n] = IP_VER4;
                    ++n;
                }
            }

            for (int s = 0; s < MAX_IPV6_PER_INTERFACE; ++s) {
                l3_ipv6_interface_t* v6 = l2->l3_v6[s];
                if (!is_valid_v6_l3_for_bind(v6)) continue;
                if (n < SOCK_MAX_L3) {
                    tmp_ids[n] = v6->l3_id;
                    tmp_ver[n] = IP_VER6;
                    ++n;
                }
            }
        }

        return n > 0;
    }

public:
    explicit TCPSocket(uint8_t r = SOCK_ROLE_CLIENT, uint32_t pid_ = 0, const SocketExtraOptions* extra = nullptr) : Socket(PROTO_TCP, r, extra) {
        pid = pid_;
        if (!(extraOpts.flags & SOCK_OPT_BUF_SIZE)) {
            extraOpts.flags |= SOCK_OPT_BUF_SIZE;
            extraOpts.buf_size = TCP_RING_CAP;
        }

        if (!extraOpts.buf_size) extraOpts.buf_size = TCP_RING_CAP;
        if (extraOpts.buf_size > TCP_RING_CAP) extraOpts.buf_size = TCP_RING_CAP;
        insert_in_list();
    }

    ~TCPSocket() override {
        close();
        remove_from_list();
    }

    int32_t bind(const SockBindSpec& spec_in, uint16_t port) override {
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_TCP;
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
        ip_version_t vers[SOCK_MAX_L3];
        int n = 0;

        if (spec.kind == BIND_L3){
            if (!spec.l3_id) return SOCK_ERR_INVAL;

            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(spec.l3_id);
            l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(spec.l3_id);

            bool ok4 = is_valid_v4_l3_for_bind(v4);
            bool ok6 = is_valid_v6_l3_for_bind(v6);

            if (!ok4 && !ok6) return SOCK_ERR_INVAL;

            if (ok4 && n < SOCK_MAX_L3) {
                ids[n] = spec.l3_id;
                vers[n] = IP_VER4;
                ++n;
            }
            if (ok6 && n < SOCK_MAX_L3) {
                ids[n] = spec.l3_id;
                vers[n] = IP_VER6;
                ++n;
            }
        } else if (spec.kind == BIND_L2){
            if (!add_all_l3_on_l2(spec.ifindex, ids, vers, n)) return SOCK_ERR_INVAL;
        } else if (spec.kind == BIND_IP){
            if (spec.ver == IP_VER4){
                uint32_t v4ip = 0;
                memcpy(&v4ip, spec.ip, 4);
                l3_ipv4_interface_t* ipif = l3_ipv4_find_by_ip(v4ip);
                if (!is_valid_v4_l3_for_bind(ipif)) return SOCK_ERR_INVAL;
                ids[n] = ipif->l3_id;
                vers[n] = IP_VER4;
                ++n;
            } else if (spec.ver == IP_VER6) {
                l3_ipv6_interface_t* ipif = l3_ipv6_find_by_ip(spec.ip);
                if (!is_valid_v6_l3_for_bind(ipif)) return SOCK_ERR_INVAL;
                ids[n] = ipif->l3_id;
                vers[n] = IP_VER6;
                ++n;
            } else return SOCK_ERR_INVAL;
        } else if (spec.kind == BIND_ANY){
            if (!add_all_l3_any(ids, vers, n)) return SOCK_ERR_INVAL;
        } else return SOCK_ERR_INVAL;

        if (n==0) return SOCK_ERR_INVAL;

        uint8_t dedup_ids[SOCK_MAX_L3];
        ip_version_t dedup_ver[SOCK_MAX_L3];
        int m = 0;

        for (int i = 0; i < n; ++i) {
            bool seen = false;
            for (int j = 0; j < m; ++j) {
                if (dedup_ids[j] == ids[i] && dedup_ver[j] == vers[i]) {
                    seen = true;
                    break;
                }
            }
            if (!seen && m < SOCK_MAX_L3) {
                dedup_ids[m] = ids[i];
                dedup_ver[m] = vers[i];
                ++m;
            }
        }

        if (m==0) return SOCK_ERR_INVAL;

        uint8_t bound_ids[SOCK_MAX_L3];
        int bdone=0;

        for (int i = 0; i < m; ++i){
            uint8_t id = dedup_ids[i];
            bool ok = tcp_bind_l3(id, port, pid, dispatch, &extraOpts);
            if (!ok){
                for (int j=0;j<bdone;++j) (void)tcp_unbind_l3(bound_ids[j], port, pid);
                return SOCK_ERR_SYS;
            }
            bound_ids[bdone] = id;
            ++bdone;
        }

        if (bdone == 0) return SOCK_ERR_SYS;

        clear_bound_l3();
        for (int i=0;i<m;++i) add_bound_l3(dedup_ids[i]);

        localPort = port;
        bound = true;
        return SOCK_OK;
    }

    int32_t listen(int max_backlog){
        if (!bound || role != SOCK_ROLE_SERVER) return SOCK_ERR_STATE;

        backlogCap = max_backlog > TCP_MAX_BACKLOG ? TCP_MAX_BACKLOG : max_backlog;
        backlogLen = 0;
        return SOCK_OK;
    }

    TCPSocket* accept(){
        const int max_iters = 100;
        int iter = 0;

        while (backlogLen == 0){
            if (++iter > max_iters) return nullptr;
            msleep(10);
        }

        TCPSocket* client = pending[0];

        for (int i = 1; i < backlogLen; ++i) pending[i - 1] = pending[i];
        pending[--backlogLen] = nullptr;

        return client;
    }

    int32_t connect(SockDstKind kind, const void* dst, uint16_t port) {
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_TCP;
        ev.action = NETLOG_ACT_CONNECT;
        ev.pid = pid;
        ev.dst_kind = kind;
        ev.u0 = port;
        if (kind == DST_ENDPOINT && dst) ev.dst_ep = *(const net_l4_endpoint*)dst;
        if (kind == DST_DOMAIN) ev.s0 = (const char*)dst;
        netlog_socket_event(&extraOpts, &ev);
        if (role != SOCK_ROLE_CLIENT) return SOCK_ERR_PERM;
        if (connected) return SOCK_ERR_STATE;
        if (!dst) return SOCK_ERR_INVAL;

        net_l4_endpoint d{};
        uint8_t chosen_l3 = 0;

        uint8_t allow_v4[SOCK_MAX_L3];
        uint8_t allow_v6[SOCK_MAX_L3];
        int n4 = 0;
        int n6 = 0;

        for (int i = 0; i < bound_l3_count; ++i) {
            uint8_t id = bound_l3[i];

            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(id);
            if (is_valid_v4_l3_for_bind(v4) && n4 < SOCK_MAX_L3) allow_v4[n4++] = id;

            l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(id);
            if (is_valid_v6_l3_for_bind(v6) && n6 < SOCK_MAX_L3) allow_v6[n6++] = id;
        }

        if (kind == DST_ENDPOINT){
            const net_l4_endpoint* ed = (const net_l4_endpoint*)dst;
            d = *ed;
            if (!d.port && port) d.port = port;
            if (!d.port) return SOCK_ERR_INVAL;

            if (d.ver == IP_VER6) {
                ipv6_tx_plan_t p6;
                if (!ipv6_build_tx_plan(d.ip, nullptr, n6 ? allow_v6 : nullptr, n6, &p6)) return SOCK_ERR_SYS;
                chosen_l3 = p6.l3_id;
            } else if (d.ver == IP_VER4) {
                uint32_t dip = 0;
                memcpy(&dip, d.ip, 4);
                ipv4_tx_plan_t p4;
                if (!ipv4_build_tx_plan(dip, nullptr, n4 ? allow_v4 : nullptr, n4, &p4)) return SOCK_ERR_SYS;
                chosen_l3 = p4.l3_id;
            } else return SOCK_ERR_INVAL;
        } else if (kind == DST_DOMAIN){
            const char* host = (const char*)dst;
            if (!port) return SOCK_ERR_INVAL;

            uint8_t v6addr[16];
            memset(v6addr, 0, 16);
            uint32_t v4addr = 0;

            dns_result_t dr6 = dns_resolve_aaaa(host, v6addr, TCP_DNS_SEL, TCP_DNS_TIMEOUT_MS);
            dns_result_t dr4 = dns_resolve_a(host, &v4addr, TCP_DNS_SEL, TCP_DNS_TIMEOUT_MS);

            if (dr6 != DNS_OK && dr4 != DNS_OK) return SOCK_ERR_DNS;

            if (dr6 == DNS_OK) {
                net_l4_endpoint d6{};
                d6.ver = IP_VER6;
                memcpy(d6.ip, v6addr, 16);
                d6.port = port;

                ipv6_tx_plan_t p6;
                if (ipv6_build_tx_plan(d6.ip, nullptr, n6 ? allow_v6 : nullptr, n6, &p6)) {
                    d = d6;
                    chosen_l3 = p6.l3_id;
                }
            }

            if (!chosen_l3 && dr4 == DNS_OK) {
                net_l4_endpoint d4{};
                make_ep(v4addr, port, IP_VER4, &d4);

                uint32_t dip = 0;
                memcpy(&dip, d4.ip, 4);
                ipv4_tx_plan_t p4;
                if (ipv4_build_tx_plan(dip, nullptr, n4 ? allow_v4 : nullptr, n4, &p4)) {
                    d = d4;
                    chosen_l3 = p4.l3_id;
                }
            }

            if (!chosen_l3) return SOCK_ERR_SYS;
        } else return SOCK_ERR_INVAL;

        if (!chosen_l3) return SOCK_ERR_SYS;

        if (d.ver == IP_VER4) {
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
            if (!is_valid_v4_l3_for_bind(v4)) return SOCK_ERR_SYS;
        } else if (d.ver == IP_VER6) {
            l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(chosen_l3);
            if (!is_valid_v6_l3_for_bind(v6)) return SOCK_ERR_SYS;
        } else return SOCK_ERR_INVAL;

        if (localPort == 0) {
            int p = tcp_alloc_ephemeral_l3(chosen_l3, pid, dispatch);
            if (p < 0) return SOCK_ERR_NO_PORT;
            localPort = (uint16_t)p;
        }

        clear_bound_l3();
        add_bound_l3(chosen_l3);
        bound = true;

        tcp_data ctx_copy{};
        if (!tcp_handshake_l3(chosen_l3, localPort, &d, &ctx_copy, pid, &extraOpts)) {
            Socket::close();
            return SOCK_ERR_SYS;
        }

        uint8_t local_ip[16];
        memset(local_ip, 0, sizeof(local_ip));
        if (d.ver == IP_VER4) {
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
            if (!is_valid_v4_l3_for_bind(v4)) {
                Socket::close();
                return SOCK_ERR_SYS;
            }
            memcpy(local_ip, &v4->ip, 4);
        } else {
            l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(chosen_l3);
            if (!is_valid_v6_l3_for_bind(v6)) {
                Socket::close();
                return SOCK_ERR_SYS;
                }
            memcpy(local_ip, v6->ip, 16);
        }

        flow = tcp_get_ctx(localPort, d.ver, local_ip, (const void*)d.ip, d.port);
        if (!flow) {
            Socket::close();
            return SOCK_ERR_SYS;
        }

        remoteEP = d;
        connected = true;
        netlog_socket_event_t ev1{};
        ev1.comp = NETLOG_COMP_TCP;
        ev1.action = NETLOG_ACT_CONNECTED;
        ev1.pid = pid;
        ev1.u0 = localPort;
        ev1.u1 = remoteEP.port;
        ev1.local_port = localPort;
        ev1.remote_ep = remoteEP;
        netlog_socket_event(&extraOpts, &ev1);
        return SOCK_OK;
    }

    int64_t send(const void* buf, uint64_t len) {
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_TCP;
        ev.action = NETLOG_ACT_SEND;
        ev.pid = pid;
        ev.u0 = (uint32_t)len;
        ev.local_port = localPort;
        ev.remote_ep = remoteEP;
        netlog_socket_event(&extraOpts, &ev);
        if (!connected || !flow) return SOCK_ERR_STATE;

        const uint8_t* p = (const uint8_t*)buf;
        uint64_t sent_total = 0;

        while (sent_total < len) {
            uint64_t remain = len - sent_total;
            uint32_t chunk = remain > UINT32_MAX ? UINT32_MAX : (uint32_t)remain;
            flow->payload.ptr = (uintptr_t)(p + sent_total);
            flow->payload.size = chunk;
            flow->flags = (1u<<PSH_F) | (1u<<ACK_F);

            tcp_result_t res = tcp_flow_send(flow);
            if (res != TCP_OK) {
                if (sent_total) return (int64_t)sent_total;
                return (int64_t)res;
            }

            uint32_t pushed = flow->payload.size;
            if (!pushed) break;
            sent_total += pushed;
        }

        if (sent_total) return (int64_t)sent_total;
        return TCP_WOULDBLOCK;
    }

    int64_t recv(void* buf, uint64_t len){
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_TCP;
        ev.action = NETLOG_ACT_RECV;
        ev.pid = pid;
        ev.u0 = (uint32_t)len;
        ev.local_port = localPort;
        ev.remote_ep = remoteEP;
        netlog_socket_event(&extraOpts, &ev);
        if (!buf || !len) return 0;

        uint8_t* out = (uint8_t*)buf;
        uint64_t n = 0;

        n = ring.pop_buf(out, len);

        if (n) {
            if (flow) tcp_flow_on_app_read(flow, (uint32_t)n);
            return (int64_t)n;
        }
        if (connected) return TCP_WOULDBLOCK;
        return 0;
    }

    int32_t close() override {
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_TCP;
        ev.action = NETLOG_ACT_CLOSE;
        ev.pid = pid;
        ev.local_port = localPort;
        ev.remote_ep = remoteEP;
        netlog_socket_event(&extraOpts, &ev);
        if (connected && flow){
            tcp_flow_close(flow);
            connected = false;
            flow = nullptr;
        }

        ring.clear();
        for (int i = 0; i < backlogLen; ++i) delete pending[i];
        backlogLen = 0;

        return Socket::close();
    }

    net_l4_endpoint get_remote_ep() const { return remoteEP; }
};