#pragma once

#include "std/memory.h"
#include "std/string.h"
#include "socket.hpp"
#include "networking/transport_layer/tcp.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/application_layer/dns.h"
#include "types.h"
#include "data_struct/ring_buffer.hpp"
#include "net/socket_types.h"
#include "networking/internet_layer/ipv4_route.h"
#include "syscalls/syscalls.h"


static constexpr int TCP_MAX_BACKLOG = 8;
static constexpr dns_server_sel_t TCP_DNS_SEL = DNS_USE_BOTH;
static constexpr uint32_t TCP_DNS_TIMEOUT_MS = 3000;

class TCPSocket : public Socket {

    inline static TCPSocket* s_list_head = nullptr;

    static constexpr int TCP_RING_CAP = 1024;
    RingBuffer<sizedptr, TCP_RING_CAP> ring;
    tcp_data* flow = nullptr;

    TCPSocket* pending[TCP_MAX_BACKLOG] = { nullptr };
    int backlogCap = 0;
    int backlogLen = 0;
    TCPSocket* next = nullptr;

    static bool contains_id(const uint8_t* arr, int n, uint8_t id){
        for (int i=0;i<n;++i) if (arr[i]==id) return true;
        return false;
    }

    static bool is_valid_v4_l3_for_bind(l3_ipv4_interface_t* v4){
        if (!v4 || !v4->l2) return false;
        if (!v4->l2->is_up) return false;
        if (v4->is_localhost) return false;
        if (v4->mode == IPV4_CFG_DISABLED) return false;
        if (v4->ip == 0) return false;
        if (!v4->port_manager) return false;
        return true;
    }

    static bool socket_matches_dst(TCPSocket* s, uint8_t ifx, ip_version_t ver, const void* dst_ip_addr, uint16_t dst_port){
        if (!s->bound) return false;
        if (s->localPort != dst_port) return false;
        for (int i=0;i<s->bound_l3_count;++i){
            uint8_t id = s->bound_l3[i];
            if (ver == IP_VER4){
                l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(id);
                if (!is_valid_v4_l3_for_bind(v4)) continue;
                if (v4->l2->ifindex != ifx) continue;
                if (v4->ip == *(const uint32_t*)dst_ip_addr) return true;
            } else if (ver == IP_VER6){
                l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(id);
                if (!v6 || !v6->l2 || !v6->l2->is_up || v6->is_localhost || v6->cfg == IPV6_CFG_DISABLE) continue;
                if (v6->l2->ifindex != ifx) continue;
                if (memcmp(v6->ip, dst_ip_addr, 16) == 0) return true;
            }
        }
        return false;
    }

    static uint8_t find_matching_l3(TCPSocket* s, uint8_t ifx, ip_version_t ver, const void* dst_ip_addr){
        for (int i=0;i<s->bound_l3_count;++i){
            uint8_t id = s->bound_l3[i];
            if (ver == IP_VER4){
                l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(id);
                if (!is_valid_v4_l3_for_bind(v4)) continue;
                if (v4->l2->ifindex != ifx) continue;
                if (v4->ip == *(const uint32_t*)dst_ip_addr) return id;
            } else if (ver == IP_VER6){
                l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(id);
                if (!v6 || !v6->l2 || !v6->l2->is_up || v6->is_localhost || v6->cfg == IPV6_CFG_DISABLE) continue;
                if (v6->l2->ifindex != ifx) continue;
                if (memcmp(v6->ip, dst_ip_addr, 16) == 0) return id;
            }
        }
        if (ver == IP_VER4){
            uint32_t v4dst; memcpy(&v4dst, dst_ip_addr, 4);
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_ip(v4dst);
            if (is_valid_v4_l3_for_bind(v4) && v4->l2 && v4->l2->ifindex == ifx) return v4->l3_id;
        }
        return 0;
    }

    static bool socket_matches_flow(TCPSocket* s, uint8_t ifx, ip_version_t ver, const void* dst_ip_addr, uint16_t dst_port, const void* src_ip_addr, uint16_t src_port){
        (void)ifx;
        if (!s->connected) return false;
        if (s->localPort != dst_port) return false;
        if (s->remoteEP.port != src_port) return false;
        if (s->remoteEP.ver != ver) return false;
        if (ver == IP_VER4){
            if (*(const uint32_t*)s->remoteEP.ip != *(const uint32_t*)src_ip_addr) return false;
        } else {
            if (memcmp(s->remoteEP.ip, src_ip_addr, 16) != 0) return false;
        }
        return true;
    }

    static void dispatch(uint8_t ifindex, ip_version_t ipver, const void* src_ip_addr, const void* dst_ip_addr,
                         uintptr_t frame_ptr, uint32_t frame_len, uint16_t src_port, uint16_t dst_port)
    {
        if (frame_len == 0){
            for (TCPSocket* srv = s_list_head; srv; srv = srv->next){
                if (srv->role != SOCK_ROLE_SERVER) continue;
                if (!socket_matches_dst(srv, ifindex, ipver, dst_ip_addr, dst_port)) continue;
                if (srv->backlogLen >= srv->backlogCap) break;
                TCPSocket* child = new TCPSocket(SOCK_ROLE_CLIENT, srv->pid);
                child->localPort = dst_port;
                child->connected = true;
                child->remoteEP.ver = ipver;
                memset(child->remoteEP.ip, 0, 16);
                if (ipver == IP_VER4) memcpy(child->remoteEP.ip, src_ip_addr, 4);
                else memcpy(child->remoteEP.ip, src_ip_addr, 16);
                child->remoteEP.port = src_port;
                uint8_t l3id = find_matching_l3(srv, ifindex, ipver, dst_ip_addr);
                child->clear_bound_l3();
                if (l3id) {
                    child->add_bound_l3(l3id);
                } else if (ipver == IP_VER4){
                    uint32_t v4dst; memcpy(&v4dst, dst_ip_addr, 4);
                    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_ip(v4dst);
                    if (is_valid_v4_l3_for_bind(v4) && v4->l2 && v4->l2->ifindex == ifindex){
                        child->add_bound_l3(v4->l3_id);
                    } else {
                        for (int i=0;i<srv->bound_l3_count;i++){
                            l3_ipv4_interface_t* sv4 = l3_ipv4_find_by_id(srv->bound_l3[i]);
                            if (sv4 && sv4->l2 && sv4->l2->is_up && sv4->l2->ifindex == ifindex){
                                child->add_bound_l3(sv4->l3_id);
                            }
                        }
                    }
                }
                child->flow = tcp_get_ctx(dst_port, ipver, child->remoteEP.ip, src_port);
                if (!child->flow){
                    child->close();
                    delete child;
                    break;
                }
                srv->pending[srv->backlogLen++] = child;
                break;
            }
            return;
        }
        for (TCPSocket* s = s_list_head; s; s = s->next){
            if (!socket_matches_flow(s, ifindex, ipver, dst_ip_addr, dst_port, src_ip_addr, src_port)) continue;
            s->on_receive(frame_ptr, frame_len);
            return;
        }
        if (frame_ptr && frame_len){
            free_sized((void*)frame_ptr, frame_len);
        }
    }

    void on_receive(uintptr_t ptr, uint32_t len) {
        uint8_t* data = (uint8_t*)malloc(len);
        if (!data) return;
        memcpy(data, (void*)ptr, len);
        sizedptr packet { (uintptr_t)data, len };
        if (!ring.push(packet)) {
            sizedptr dropped;
            ring.pop(dropped);
            free_sized((void*)dropped.ptr, dropped.size);
            ring.push(packet);
        }
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
            if (*cur == this) { *cur = (*cur)->next; break; }
            cur = &((*cur)->next);
        }
        next = nullptr;
    }

    void do_unbind_one(uint8_t l3_id, uint16_t port, uint16_t pid_) override {
        (void)pid_;
        if (role != SOCK_ROLE_SERVER) return;
        (void)tcp_unbind_l3(l3_id, port, pid);
    }

    bool add_all_l3_on_l2(uint8_t ifindex, uint8_t* tmp_ids, int& n){
        l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
        if (!l2 || !l2->is_up) return false;
        for (int s=0; s<MAX_IPV4_PER_INTERFACE; ++s){
            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
            if (is_valid_v4_l3_for_bind(v4)){ if (n < SOCK_MAX_L3) tmp_ids[n++] = v4->l3_id; }
        }
        return n > 0;
    }

    bool add_all_l3_any(uint8_t* tmp_ids, int& n){
        uint8_t cnt = l2_interface_count();
        for (uint8_t i=0;i<cnt;++i){
            l2_interface_t* l2 = l2_interface_at(i);
            if (!l2 || !l2->is_up) continue;
            for (int s=0; s<MAX_IPV4_PER_INTERFACE; ++s){
                l3_ipv4_interface_t* v4 = l2->l3_v4[s];
                if (is_valid_v4_l3_for_bind(v4)){ if (n < SOCK_MAX_L3) tmp_ids[n++] = v4->l3_id; }
            }
        }
        return n > 0;
    }

    static bool is_zero_ip16(const uint8_t ip[16]){
        for (int i=0;i<16;++i) if (ip[i]) return false;
        return true;
    }

public:
    explicit TCPSocket(uint8_t r = SOCK_ROLE_CLIENT, uint32_t pid_ = 0) : Socket(PROTO_TCP, r){
        pid = pid_;
        insert_in_list();
    }

    ~TCPSocket() override {
        close();
        remove_from_list();
    }

    int32_t bind(const SockBindSpec& spec_in, uint16_t port) override {
        if (role != SOCK_ROLE_SERVER) return SOCK_ERR_PERM;
        if (bound) return SOCK_ERR_BOUND;

        SockBindSpec spec = spec_in;
        bool empty = spec.kind == BIND_L3 && spec.l3_id==0 && spec.ifindex==0 && spec.ver==0 && is_zero_ip16(spec.ip);
        if (empty) spec.kind = BIND_ANY;

        uint8_t ids[SOCK_MAX_L3];
        int n = 0;

        if (spec.kind == BIND_L3){
            if (spec.l3_id) ids[n++] = spec.l3_id;
            if (n==0) return SOCK_ERR_INVAL;
        } else if (spec.kind == BIND_L2){
            if (!add_all_l3_on_l2(spec.ifindex, ids, n)) return SOCK_ERR_INVAL;
        } else if (spec.kind == BIND_IP){
            if (spec.ver == IP_VER4){
                uint32_t v4; memcpy(&v4, spec.ip, 4);
                l3_ipv4_interface_t* ipif = l3_ipv4_find_by_ip(v4);
                if (!is_valid_v4_l3_for_bind(ipif)) return SOCK_ERR_INVAL;
                ids[n++] = ipif->l3_id;
            } else if (spec.ver == IP_VER6){
                return SOCK_ERR_PROTO;
            } else return SOCK_ERR_INVAL;
        } else if (spec.kind == BIND_ANY){
            if (!add_all_l3_any(ids, n)) return SOCK_ERR_INVAL;
        } else return SOCK_ERR_INVAL;

        if (n==0) return SOCK_ERR_INVAL;

        uint8_t dedup[SOCK_MAX_L3]; int m=0;
        for (int i=0;i<n;++i) if (!contains_id(dedup, m, ids[i])) dedup[m++] = ids[i];
        if (m==0) return SOCK_ERR_INVAL;

        uint8_t bound_ids[SOCK_MAX_L3]; int bdone=0;
        for (int i=0;i<m;++i){
            uint8_t id = dedup[i];
            bool ok = tcp_bind_l3(id, port, pid, dispatch);
            if (!ok){
                for (int j=0;j<bdone;++j) (void)tcp_unbind_l3(bound_ids[j], port, pid);
                return SOCK_ERR_SYS;
            }
            bound_ids[bdone++] = id;
        }
        if (bdone == 0) return SOCK_ERR_SYS;

        clear_bound_l3();
        for (int i=0;i<m;++i) add_bound_l3(dedup[i]);

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
        for (int i=1;i<backlogLen;++i) pending[i-1] = pending[i];
        pending[--backlogLen] = nullptr;
        return client;
    }

    int32_t connect(SockDstKind kind, const void* dst, uint16_t port){
        if (role != SOCK_ROLE_CLIENT) return SOCK_ERR_PERM;
        if (connected) return SOCK_ERR_STATE;
        if (!dst) return SOCK_ERR_INVAL;

        net_l4_endpoint d{};
        if (kind == DST_ENDPOINT){
            const net_l4_endpoint* ed = (const net_l4_endpoint*)dst;
            d = *ed;
            if (!d.port && port) d.port = port;
            if (!d.port) return SOCK_ERR_INVAL;
        } else if (kind == DST_DOMAIN){
            const char* host = (const char*)dst;
            if (!port) return SOCK_ERR_INVAL;
            uint32_t v4 = 0;
            dns_result_t dr = dns_resolve_a(host, &v4, TCP_DNS_SEL, TCP_DNS_TIMEOUT_MS);
            if (dr != DNS_OK) return SOCK_ERR_DNS;
            d.ver = IP_VER4; memset(d.ip, 0, 16); memcpy(d.ip, &v4, 4); d.port = port;
        } else return SOCK_ERR_INVAL;

        if (d.ver == IP_VER6) return SOCK_ERR_PROTO;
        if (d.ver != IP_VER4) return SOCK_ERR_INVAL;

        uint8_t chosen_l3 = 0;

        if (bound_l3_count == 0){
            uint8_t ids[SOCK_MAX_L3]; int n = 0;
            uint8_t cnt = l2_interface_count();
            for (uint8_t i = 0; i < cnt; ++i){
                l2_interface_t* l2 = l2_interface_at(i);
                if (!l2 || !l2->is_up) continue;
                for (int s = 0; s < MAX_IPV4_PER_INTERFACE && n < SOCK_MAX_L3; ++s){
                    l3_ipv4_interface_t* v4 = l2->l3_v4[s];
                    if (!is_valid_v4_l3_for_bind(v4)) continue;
                    ids[n++] = v4->l3_id;
                }
            }
            if (n == 0) return SOCK_ERR_SYS;
            uint32_t dip; memcpy(&dip, d.ip, 4);
            if (!ipv4_rt_pick_best_l3_in(ids, n, dip, &chosen_l3)) return SOCK_ERR_SYS;
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
            if (!is_valid_v4_l3_for_bind(v4)) return SOCK_ERR_SYS;
            int p = tcp_alloc_ephemeral_l3(chosen_l3, pid, dispatch);
            if (p < 0) return SOCK_ERR_NO_PORT;
            localPort = (uint16_t)p;
            clear_bound_l3();
            add_bound_l3(chosen_l3);
        } else if (bound_l3_count == 1){
            chosen_l3 = bound_l3[0];
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
            if (!is_valid_v4_l3_for_bind(v4)) return SOCK_ERR_SYS;
            if (localPort == 0){
                int p = tcp_alloc_ephemeral_l3(chosen_l3, pid, dispatch);
                if (p < 0) return SOCK_ERR_NO_PORT;
                localPort = (uint16_t)p;
            }
        } else {
            uint32_t dip; memcpy(&dip, d.ip, 4);
            if (!ipv4_rt_pick_best_l3_in(bound_l3, bound_l3_count, dip, &chosen_l3)) return SOCK_ERR_SYS;
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
            if (!is_valid_v4_l3_for_bind(v4)) return SOCK_ERR_SYS;
            if (localPort == 0){
                int p = tcp_alloc_ephemeral_l3(chosen_l3, pid, dispatch);
                if (p < 0) return SOCK_ERR_NO_PORT;
                localPort = (uint16_t)p;
            }
        }

        tcp_data ctx_copy{};
        if (!tcp_handshake_l3(chosen_l3, localPort, &d, &ctx_copy, pid)) return SOCK_ERR_SYS;

        flow = tcp_get_ctx(localPort, d.ver, (const void*)d.ip, d.port);
        if (!flow) return SOCK_ERR_SYS;

        remoteEP = d;
        connected = true;
        return SOCK_OK;
    }

    int64_t send(const void* buf, uint64_t len){
        if (!connected || !flow) return SOCK_ERR_STATE;
        flow->payload = { (uintptr_t)buf, (uint32_t)len };
        flow->flags = (1u<<PSH_F) | (1u<<ACK_F);
        tcp_result_t res = tcp_flow_send(flow);
        return (res == TCP_OK) ? (int64_t)len : (int64_t)res;
    }

    int64_t recv(void* buf, uint64_t len){
        sizedptr p;
        if (!ring.pop(p)) return 0;
        uint32_t tocpy = p.size < (uint32_t)len ? p.size : (uint32_t)len;
        memcpy(buf, (void*)p.ptr, tocpy);
        free_sized((void*)p.ptr, p.size);
        return tocpy;
    }

    int32_t close() override {
        if (connected && flow){
            tcp_flow_close(flow);
            connected = false;
            flow = nullptr;
        }
        sizedptr pkt;
        while (ring.pop(pkt)){ free_sized((void*)pkt.ptr, pkt.size); }
        for (int i=0;i<backlogLen;++i){ delete pending[i]; }
        backlogLen = 0;

        if (role == SOCK_ROLE_SERVER){
            return Socket::close();
        } else {
            bound = false;
            localPort = 0;
            clear_bound_l3();
            return SOCK_OK;
        }
    }

    net_l4_endpoint get_remote_ep() const { return remoteEP; }
};
