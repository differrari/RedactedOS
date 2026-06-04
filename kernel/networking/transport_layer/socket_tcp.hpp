#pragma once

#include "std/memory.h"
#include "std/string.h"
#include "socket.hpp"
#include "networking/transport_layer/tcp.h"
#include "networking/transport_layer/csocket_tcp.h"
#include "networking/internet_layer/ipv4.h"
#include "types.h"
#include "net/socket_types.h"
#include "networking/internet_layer/ipv4_route.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_route.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/transport_layer/trans_utils.h"
#include "syscalls/syscalls.h"

static constexpr int TCP_MAX_BACKLOG = 128;

class TCPSocket : public Socket {
    static constexpr uint32_t TCP_DEFAULT_SOCKET_BUF = 256 * 1024;

    tcp_data flow = {};

    ksocket_t* pending[TCP_MAX_BACKLOG] = { nullptr };
    int backlogCap = 0;
    int backlogLen = 0;

    static bool is_valid_v4_l3_for_bind(l3_ipv4_interface_t* v4) {
        if (!v4 || !v4->l2) return false;
        if (!v4->l2->is_up) return false;
        if (v4->mode == IPV4_CFG_DISABLED) return false;
        if (v4->ip == 0) return false;
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
        return true;
    }

public:
    uint32_t queue_accepted_child(uint8_t ifindex, ip_version_t ipver, const void* src_ip_addr, const void* dst_ip_addr, uint16_t src_port, uint16_t dst_port) {
        if (role != SOCKET_SERVER || !bound || localPort != dst_port) return 0;

        for (int i = 0; i < backlogLen;) {
            ksocket_t* owner = pending[i];
            TCPSocket* client = owner ? (TCPSocket*)socket_core_impl(owner) : nullptr;
            if (client && client->flow.flow_generation) {
                bool readable = tcp_flow_readable(&client->flow) != 0;
                bool closed = tcp_flow_recv_closed(&client->flow);
                if (!closed || readable) {
                    ++i;
                    continue;
                }
            }

            owner = pop_pending_at(i);
            if (owner) {
                socket_core_close_socket(owner);
                socket_core_put(owner);
            }
        }
        if (backlogLen >= backlogCap) return 0;

        ksocket_t* child_owner = nullptr;
        if (!socket_core_alloc(SOCKET_CLIENT, PROTO_TCP, pid, &child_owner)) return 0;

        TCPSocket* child = new TCPSocket(child_owner, SOCKET_CLIENT, pid, &extraOpts);
        if (!child) {
            socket_core_close_socket(child_owner);
            return 0;
        }

        child->localPort = dst_port;

        child->bound = true;
        child->remoteEP.ver = ipver;
        memset(child->remoteEP.ip, 0, 16);
        if (ipver == IP_VER4) memcpy(child->remoteEP.ip, src_ip_addr, 4);
        else memcpy(child->remoteEP.ip, src_ip_addr, 16);
        child->remoteEP.port = src_port;
        child->bindSpec.kind = BIND_IP;
        child->bindSpec.ver = ipver;
        if (ipver == IP_VER4) memcpy(child->bindSpec.ip, dst_ip_addr, 4);
        else if (ipver == IP_VER6) memcpy(child->bindSpec.ip, dst_ip_addr, 16);

        if (!tcp_get_ctx(dst_port, ipver, dst_ip_addr, child->remoteEP.ip, src_port, &child->flow)) {
            delete child;
            socket_core_close_socket(child_owner);
            return 0;
        }

        if (!socket_core_attach_impl(child_owner, child, socket_destroy_tcp, socket_close_tcp, socket_setopt_tcp, socket_getopt_tcp, nullptr)) {
            delete child;
            socket_core_close_socket(child_owner);
            return 0;
        }

        child->connected = true;
        socket_core_ref(child_owner);
        pending[backlogLen++] = child_owner;
        return 1;
    }

private:

    ksocket_t* pop_pending_at(int idx) {
        if (idx < 0 || idx >= backlogLen) return nullptr;

        ksocket_t* client = pending[idx];
        for (int i = idx + 1; i < backlogLen; ++i) pending[i-1] = pending[i];

        pending[--backlogLen] = nullptr;
        return client;
    }

public:
    explicit TCPSocket(ksocket_t* owner, uint8_t r = SOCKET_CLIENT, uint32_t pid_ = 0, const SocketExtraOptions* extra = nullptr) : Socket(owner, PROTO_TCP, r, extra) {
        pid = pid_;
        if (!(extraOpts.flags & SOCK_OPT_BUF_SIZE)) {
            extraOpts.flags |= SOCK_OPT_BUF_SIZE;
            extraOpts.buf_size = TCP_DEFAULT_SOCKET_BUF;
        }

        if (!extraOpts.buf_size) extraOpts.buf_size = TCP_DEFAULT_SOCKET_BUF;
        if (extraOpts.buf_size > TCP_DEFAULT_SOCKET_BUF) extraOpts.buf_size = TCP_DEFAULT_SOCKET_BUF;
    }

    ~TCPSocket() override {
        close();
    }

    int32_t set_option(int32_t opt, const void* value, uint32_t len) {
        if (!value || len != sizeof(uint32_t)) return SOCK_ERR_INVAL;

        uint32_t v = 0;
        memcpy(&v, value, sizeof(v));

        switch ((uint32_t)opt) {
            case SOCK_OPT_KEEPALIVE:
                if (v) {
                    extraOpts.flags |= SOCK_OPT_KEEPALIVE;
                    if (!extraOpts.keepalive_ms) extraOpts.keepalive_ms = SOCKET_DEFAULT_KEEPALIVE_MS;
                } else extraOpts.flags &= ~SOCK_OPT_KEEPALIVE;
                if (flow.flow_generation) tcp_flow_apply_socket_options(&flow, &extraOpts);
                return SOCK_OK;
            case SOCK_OPT_KEEPALIVE_INTERVAL:
                if (!v) return SOCK_ERR_INVAL;
                extraOpts.keepalive_ms = v;
                extraOpts.flags |= SOCK_OPT_KEEPALIVE | SOCK_OPT_KEEPALIVE_INTERVAL;
                if (flow.flow_generation) tcp_flow_apply_socket_options(&flow, &extraOpts);
                return SOCK_OK;
            case SOCK_OPT_TCP_NO_DELAY:
                if (v) extraOpts.flags |= SOCK_OPT_TCP_NO_DELAY;
                else extraOpts.flags &= ~SOCK_OPT_TCP_NO_DELAY;
                if (flow.flow_generation) tcp_flow_apply_socket_options(&flow, &extraOpts);
                return SOCK_OK;
            case SOCK_OPT_SEND_BUF_SIZE:
                if (!v) return SOCK_ERR_INVAL;
                extraOpts.send_buf_size = v;
                extraOpts.flags |= SOCK_OPT_SEND_BUF_SIZE;
                if (flow.flow_generation) tcp_flow_apply_socket_options(&flow, &extraOpts);
                return SOCK_OK;
            case SOCK_OPT_MCAST_JOIN:
                return SOCK_ERR_INVAL;
            default: {
                int32_t ret = Socket::set_option(opt, value, len);
                if (ret == SOCK_OK && flow.flow_generation) tcp_flow_apply_socket_options(&flow, &extraOpts);
                return ret;
            }
        }
    }

    int32_t get_option(int32_t opt, void* value, uint32_t* len) const {
        if (!value || !len || *len < sizeof(uint32_t)) return SOCK_ERR_INVAL;

        uint32_t v = 0;
        switch ((uint32_t)opt) {
            case SOCK_OPT_KEEPALIVE:
                v = (extraOpts.flags & SOCK_OPT_KEEPALIVE) != 0;
                break;
            case SOCK_OPT_KEEPALIVE_INTERVAL:
                v = extraOpts.keepalive_ms;
                break;
            case SOCK_OPT_TCP_NO_DELAY:
                v = (extraOpts.flags & SOCK_OPT_TCP_NO_DELAY) != 0;
                break;
            case SOCK_OPT_SEND_BUF_SIZE:
                v = extraOpts.send_buf_size;
                break;
            case SOCK_OPT_MCAST_JOIN:
                return SOCK_ERR_INVAL;
            default:
                return Socket::get_option(opt, value, len);
        }

        memcpy(value, &v, sizeof(v));
        *len = sizeof(uint32_t);
        return SOCK_OK;
    }

    int32_t bind(const SockBindSpec& spec_in, uint16_t port) override {
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_TCP;
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

        if (spec.kind == BIND_L3){
            if (!spec.l3_id) return SOCK_ERR_INVAL;
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(spec.l3_id);
            l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(spec.l3_id);
            bool ok4 = (spec.ver == 0 || spec.ver == IP_VER4) && is_valid_v4_l3_for_bind(v4);
            bool ok6 = (spec.ver == 0 || spec.ver == IP_VER6) && is_valid_v6_l3_for_bind(v6);
            if (!ok4 && !ok6) return SOCK_ERR_INVAL;
        } else if (spec.kind == BIND_L2) {
            l2_interface_t* l2 = l2_interface_find_by_index(spec.ifindex);
            if (!l2 || !l2->is_up) return SOCK_ERR_INVAL;
        } else if (spec.kind == BIND_IP){
            if (spec.ver == IP_VER4){
                uint32_t v4ip = 0;
                memcpy(&v4ip, spec.ip, 4);
                if (!ipv4_is_unspecified(v4ip) && !is_valid_v4_l3_for_bind(l3_ipv4_find_by_ip(v4ip))) return SOCK_ERR_INVAL;
            } else if (spec.ver == IP_VER6) {
                if (!ipv6_is_unspecified(spec.ip) && !is_valid_v6_l3_for_bind(l3_ipv6_find_by_ip(spec.ip))) return SOCK_ERR_INVAL;
            } else return SOCK_ERR_INVAL;
        } else if (spec.kind != BIND_ANY && spec.kind != BIND_ANY4 && spec.kind != BIND_ANY6) return SOCK_ERR_INVAL;

        if (!socket_bind_insert(ownerSocket, PROTO_TCP, &spec, port)) return SOCK_ERR_SYS;

        bindSpec = spec;
        localPort = port;
        bound = true;
        return SOCK_OK;
    }

    int32_t listen(int max_backlog){
        if (!bound || role != SOCKET_SERVER) return SOCK_ERR_STATE;

        backlogCap = max_backlog > TCP_MAX_BACKLOG ? TCP_MAX_BACKLOG : max_backlog;
        if (backlogCap < 1) backlogCap = 1;
        backlogLen = 0;
        return SOCK_OK;
    }

    ksocket_t* accept(){
        const int max_empty_iters = 200;
        const int ready_wait_iters = 4;
        int iter = 0;

        while (backlogLen == 0){
            if (++iter > max_empty_iters) return nullptr;
            msleep(5);
        }

        iter = 0;
        while (1) {
            for (int i = 0; i < backlogLen;) {
                ksocket_t* owner = pending[i];
                TCPSocket* client = owner ? (TCPSocket*)socket_core_impl(owner) : nullptr;

                if (client && client->flow.flow_generation) {
                    bool readable = tcp_flow_readable(&client->flow) != 0;
                    bool closed = tcp_flow_recv_closed(&client->flow);

                    if (!closed || readable) {
                        i++;
                        continue;
                    }
                }

                owner = pop_pending_at(i);
                if (owner) {
                    socket_core_close_socket(owner);
                    socket_core_put(owner);
                }
            }

            if (backlogLen == 0) return nullptr;
            for (int i = 0; i < backlogLen; ++i) {
                ksocket_t* owner = pending[i];
                TCPSocket* client = owner ? (TCPSocket*)socket_core_impl(owner) : nullptr;
                if (!client) continue;
                if (!client->flow.flow_generation || tcp_flow_readable(&client->flow) || tcp_flow_recv_closed(&client->flow)) return pop_pending_at(i);
            }

            if (++iter > ready_wait_iters) {
                if (backlogLen >= backlogCap) {
                    ksocket_t* owner = pop_pending_at(0);
                    if (owner) {
                        socket_core_close_socket(owner);
                        socket_core_put(owner);
                    }
                }
                return nullptr;
            }
            msleep(5);
        }
    }

    int32_t connect(const net_l4_endpoint* dst) {
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_TCP;
        ev.action = NETLOG_ACT_CONNECT;
        ev.pid = pid;
        if (dst) ev.dst_ep = *dst;
        netlog_socket_event(&extraOpts, &ev);
        if (role != SOCKET_CLIENT) return SOCK_ERR_PERM;
        if (connected) return SOCK_ERR_STATE;
        if (!dst || !dst->port) return SOCK_ERR_INVAL;

        net_l4_endpoint d = *dst;
        uint8_t chosen_l3 = 0;

        uint8_t allow_v4[MAX_L2_INTERFACES * MAX_IPV4_PER_INTERFACE];
        uint8_t allow_v6[MAX_L2_INTERFACES * MAX_IPV6_PER_INTERFACE];
        uint32_t n4 = bound ? socket_bind_l3_list(&bindSpec, IP_VER4, allow_v4, MAX_L2_INTERFACES * MAX_IPV4_PER_INTERFACE) : 0;
        uint32_t n6 = bound ? socket_bind_l3_list(&bindSpec, IP_VER6, allow_v6, MAX_L2_INTERFACES * MAX_IPV6_PER_INTERFACE) : 0;

        if (d.ver == IP_VER6) {
            if (bound && n6 == 0) return SOCK_ERR_SYS;
            ipv6_tx_plan_t p6;
            if (!ipv6_build_tx_plan(d.ip, nullptr, n6 ? allow_v6 : nullptr, n6, &p6)) return SOCK_ERR_SYS;
            chosen_l3 = p6.l3_id;
        } else if (d.ver == IP_VER4) {
            if (bound && n4 == 0) return SOCK_ERR_SYS;
            uint32_t dip = 0;
            memcpy(&dip, d.ip, 4);
            ipv4_tx_plan_t p4;
            if (!ipv4_build_tx_plan(dip, nullptr, n4 ? allow_v4 : nullptr, n4, &p4)) return SOCK_ERR_SYS;
            chosen_l3 = p4.l3_id;
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
            if (!ownerSocket) return SOCK_ERR_SYS;
            int p = socket_bind_alloc_ephemeral_l3(ownerSocket, PROTO_TCP, chosen_l3, pid);
            if (p < 0) return SOCK_ERR_NO_PORT;
            localPort = (uint16_t)p;
        }

        bindSpec = {};
        bindSpec.kind = BIND_L3;
        bindSpec.ver = d.ver;
        bindSpec.l3_id = chosen_l3;
        bound = true;

        flow = tcp_data{};
        if (!tcp_handshake_l3(chosen_l3, localPort, &d, &flow, pid, &extraOpts)) {
            Socket::close();
            return SOCK_ERR_SYS;
        }

        if (d.ver == IP_VER4) {
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
            if (!is_valid_v4_l3_for_bind(v4)) {
                Socket::close();
                return SOCK_ERR_SYS;
            }
        } else {
            l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(chosen_l3);
            if (!is_valid_v6_l3_for_bind(v6)) {
                Socket::close();
                return SOCK_ERR_SYS;
            }
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
        if (!connected || !flow.flow_generation) return SOCK_ERR_STATE;
        if (!buf && len) return SOCK_ERR_INVAL;
        if (!len) return 0;

        uint32_t chunk = len > UINT32_MAX ? UINT32_MAX : (uint32_t)len;
        flow.payload.ptr = (uintptr_t)buf;
        flow.payload.size = chunk;
        flow.flags = (1u<<PSH_F) | (1u<<ACK_F);

        tcp_result_t res = tcp_flow_send(&flow);
        if (res != TCP_OK) return (int64_t)res;
        if (flow.payload.size) return (int64_t)flow.payload.size;
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
        if (!connected || !flow.flow_generation) return connected ? TCP_WOULDBLOCK : 0;

        int64_t n = tcp_flow_read(&flow, buf, len);
        if (n != 0) return n;
        if (tcp_flow_recv_closed(&flow)) return 0;
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
        if (connected && flow.flow_generation){
            if (tcp_flow_is_closed(&flow)) tcp_flow_release_closed(&flow);
            else {
                tcp_flow_flush(&flow);
                tcp_flow_close(&flow);
            }
            connected = false;
            flow = tcp_data{};
        }

        for (int i = 0; i < backlogLen; ++i) {
            if (!pending[i]) continue;
            socket_core_close_socket(pending[i]);
            socket_core_put(pending[i]);
            pending[i] = nullptr;
        }
        backlogLen = 0;

        return Socket::close();
    }

    net_l4_endpoint get_remote_ep() const { return remoteEP; }
};