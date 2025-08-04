#pragma once

#include "console/kio.h"
#include "std/string.h"
#include "net/internet_layer/ipv4.h"
#include "std/memfunctions.h"
#include "socket.hpp"
#include "net/transport_layer/tcp.h"
#include "types.h"
#include "data_struct/ring_buffer.hpp"

#define KP(fmt, ...) \
    do { kprintf(fmt, ##__VA_ARGS__); } while (0)
    
extern "C" {
    void      sleep(uint64_t ms);
    uintptr_t malloc(uint64_t size);
    void      free(void *ptr, uint64_t size);
}

static constexpr int TCP_MAX_BACKLOG = 8;

class TCPSocket : public Socket {

    inline static TCPSocket* s_by_port[MAX_PORTS] = { nullptr };
    inline static TCPSocket* s_list_head = nullptr;

    static constexpr int TCP_RING_CAP = 1024;
    RingBuffer<sizedptr, TCP_RING_CAP> ring;
    tcp_data* flow = nullptr;

    TCPSocket* pending[TCP_MAX_BACKLOG] = { nullptr };
    int backlogCap = 0;
    int backlogLen = 0;

    static void dispatch(uintptr_t ptr,
                        uint32_t len,
                        uint32_t src_ip,
                        uint16_t src_port,
                        uint16_t dst_port)
    {
        if (len == 0) {
            TCPSocket* srv = s_by_port[dst_port];
            if (!srv || srv->role != SOCK_ROLE_SERVER) return;
            if (srv->backlogLen >= srv->backlogCap) return;

            TCPSocket* child = new TCPSocket();
            child->localPort = dst_port;
            child->remoteIP = src_ip;
            child->remotePort = src_port;
            child->bound = true;
            child->connected = true;
            child->flow = tcp_get_ctx(dst_port, src_ip, src_port);
            child->pid = srv->pid;
            child->insert_in_global_list();
            srv->pending[srv->backlogLen++] = child;
            return;
        }

        for (TCPSocket* s = s_list_head; s; s = s->next) {
            if (s->connected &&
                s->localPort  == dst_port &&
                s->remoteIP == src_ip &&
                s->remotePort == src_port)
            {
                s->on_receive(ptr, len);
                return;
            }
        }
    }

    void on_receive(uintptr_t ptr, uint32_t len) {
        auto data = reinterpret_cast<uint8_t*>(malloc(len));
        if (!data) return;

        memcpy(data, (void*)ptr, len);
        sizedptr packet = { (uintptr_t)data, len };

        if (!ring.push(packet)) {
            sizedptr dropped;
            ring.pop(dropped);
            free((void*)dropped.ptr, dropped.size);
            ring.push(packet);
        }
    }

    void insert_in_global_list() {
        next = s_list_head;
        s_list_head = this;
    }

    void remove_from_global_list() {
        TCPSocket** cur = &s_list_head;
        while (*cur) {
            if (*cur == this) {
                *cur = (*cur)->next;
                break;
            }
            cur = &((*cur)->next);
        }
    }

    TCPSocket* next =nullptr;

public:
    explicit TCPSocket(uint8_t r = SOCK_ROLE_CLIENT, uint32_t pid_ = 0)
      : Socket(PROTO_TCP, r)
    {
        if (pid_ != 0) {
            pid = pid_;
            insert_in_global_list();
        }
    }

    ~TCPSocket() override {
        close();
        remove_from_global_list();
    }

    int32_t bind(uint16_t port) override {
        if (role != SOCK_ROLE_SERVER) return SOCK_ERR_PERM;
        if (bound) return SOCK_ERR_BOUND;
        if (!tcp_bind(port, pid, dispatch))
            return SOCK_ERR_SYS;
        s_by_port[port] = this;
        localPort = port; bound = true;
        return SOCK_OK;
    }

    int32_t connect(uint32_t ip, uint16_t port) {
        if (role != SOCK_ROLE_CLIENT) return SOCK_ERR_PERM;
        if (connected) return SOCK_ERR_STATE;

        int p = tcp_alloc_ephemeral(pid, dispatch);
        if (p < 0) return SOCK_ERR_NO_PORT;
        s_by_port[p] = this;
        localPort = p; bound = true;

        tcp_data ctx_copy{};
        net_l4_endpoint dst{ip, port};
        if (!tcp_handshake(p, &dst, &ctx_copy, 0)) return SOCK_ERR_SYS;

        flow = tcp_get_ctx(p, ip, port);
        if (!flow) return SOCK_ERR_SYS;

        remoteIP = ip;
        remotePort = port;
        connected = true;
        return SOCK_OK;
    }

    int64_t send(const void* buf, uint64_t len) {
        if (!connected || !flow) return SOCK_ERR_STATE;
        flow->payload = { (uintptr_t)buf, (uint32_t)len };
        flow->flags = (1<<PSH_F) | (1<<ACK_F);
        tcp_result_t res = tcp_flow_send(flow);
        return (res == TCP_OK) ? (int64_t)len : res;
    }

    int64_t recv(void* buf, uint64_t len) {
        sizedptr p;
        if (!ring.pop(p)) return 0;
        uint32_t tocpy = p.size < (uint32_t)len ? p.size : (uint32_t)len;
        memcpy(buf, (void*)p.ptr, tocpy);
        free((void*)p.ptr, p.size);
        return tocpy;
    }

    int32_t listen(int max_backlog) {
        if (!bound || role != SOCK_ROLE_SERVER) return SOCK_ERR_STATE;
        backlogCap = max_backlog > TCP_MAX_BACKLOG ? TCP_MAX_BACKLOG : max_backlog;
        backlogLen = 0;
        return SOCK_OK;
    }

    TCPSocket* accept() {
        const int max_iters = 100;
        int iter = 0;
        while (backlogLen == 0) {
            if (++iter > max_iters) return nullptr;
            sleep(10);
        }
        TCPSocket* client = pending[0];
        for (int i = 1; i < backlogLen; ++i)
            pending[i - 1] = pending[i];
        pending[--backlogLen] = nullptr;
        return client;
    }

    int32_t close_client() {
        
        if (connected && flow) {
            tcp_flow_close(flow);
            connected = false;
        }
        if (bound) {
            if (s_by_port[localPort] == this) {
                tcp_unbind(localPort, pid);
                s_by_port[localPort] = nullptr;
            }
            bound = false;
        }
        sizedptr pkt;
        while (ring.pop(pkt)) {
            free((void*)pkt.ptr, pkt.size);
        }
        return SOCK_OK;
    }

    int32_t close_server() {
        if (bound) {
            tcp_unbind(localPort, pid);
            s_by_port[localPort] = nullptr;
            bound = false;
        }
        for (int i = 0; i < backlogLen; ++i) {
            delete pending[i];
        }
        backlogLen = 0;
        sizedptr pkt;
        while (ring.pop(pkt)) {
            free((void*)pkt.ptr, pkt.size);
        }
        return SOCK_OK;
    }

    int32_t close() override {
        return role == SOCK_ROLE_SERVER ? close_server() : close_client();
    }
};
