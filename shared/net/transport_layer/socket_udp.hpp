#pragma once

#include "socket.hpp"
#include "net/transport_layer/udp.h"
#include "types.h"
#include "std/string.h"
#include "net/internet_layer/ipv4.h"
#include "std/memfunctions.h"

extern "C" {
    void      sleep(uint64_t ms);
    uintptr_t malloc(uint64_t size);
    void      free(void *ptr, uint64_t size);
}

static constexpr int32_t UDP_RING_CAP = 1024;

class UDPSocket : public Socket {
    static UDPSocket* s_by_port[MAX_PORTS];

    sizedptr ring[UDP_RING_CAP];
    uint32_t src_ips[UDP_RING_CAP];
    uint16_t src_ports[UDP_RING_CAP];

    int32_t r_head = 0, r_tail = 0;

    static void dispatch(uintptr_t ptr, uint32_t len, //b UDPSocket::dispatch
                         uint32_t src_ip, uint16_t src_port, uint16_t dst_port) {
        auto *sock = s_by_port[dst_port];
        if (!sock) return;

        if (sock->remotePort != 0 && sock->remotePort != src_port)return;

        //if (sock->remoteIP  != 0 && sock->remoteIP  != src_ip)return;

        sock->on_receive(ptr, len, src_ip, src_port);
    }

    void on_receive(uintptr_t ptr,
                    uint32_t len,
                    uint32_t src_ip,
                    uint16_t src_port)
    {
        this->remoteIP = src_ip;
        this->remotePort = src_port;

        uintptr_t copy = malloc(len);
        if (!copy) {
            free((void*)ptr, len);
            return;
        }
        memcpy((void*)copy, (void*)ptr, len);
        free((void*)ptr, len);

        int next = (r_tail + 1) % UDP_RING_CAP;
        if (next == r_head) {
            free((void*)ring[r_head].ptr, ring[r_head].size);
            r_head = (r_head + 1) % UDP_RING_CAP;
        }

        ring[r_tail] = { (uintptr_t)copy, len };
        src_ips[r_tail] = src_ip;
        src_ports[r_tail] = src_port;
        r_tail  = next;
    }

public:
    UDPSocket(uint8_t r, uint32_t pid_) : Socket(PROTO_UDP, r) {
        this->pid = pid_;
        this->role = r;
        this->proto = PROTO_UDP;
        if (this->role == SOCK_ROLE_CLIENT) {
            int p = udp_alloc_ephemeral(pid_, dispatch);
            if (p >= 0) {
                s_by_port[p] = this;
                this->localPort = p;
                this->bound = true;
            }
        }
    }
    
    ~UDPSocket() override { close(); }

    int32_t bind(uint16_t port) override {
        if (role != SOCK_ROLE_SERVER) return SOCK_ERR_PERM;
        if (bound)                    return SOCK_ERR_BOUND;
        if (!udp_bind(port, pid, dispatch))
            return SOCK_ERR_SYS;
        s_by_port[port] = this;
        this->localPort = port;
        this->bound = true;
        return SOCK_OK;
    }
    
    int64_t sendto(uint32_t ip, uint16_t port, const void* buf, uint64_t len) {
        if (!bound) return SOCK_ERR_NOT_BOUND;
        net_l4_endpoint src{ ipv4_get_cfg()->ip, localPort };
        net_l4_endpoint dst{ ip, port };
        sizedptr pay{ (uintptr_t)buf, (uint32_t)len };
        udp_send_segment(&src, &dst, pay);
        this->remoteIP   = ip;
        this->remotePort = port;
        return (int64_t)len;
    }

    int64_t recvfrom(void* buf, uint64_t len,   //b UDPSocket::recvfrom
                      uint32_t* src_ip,
                      uint16_t* src_port)
    {
        if (r_head == r_tail)return 0;

        auto     p  = ring[r_head];
        uint32_t ip = src_ips[r_head];
        uint16_t pt = src_ports[r_head];
        r_head = (r_head + 1) % UDP_RING_CAP;

        uint32_t tocpy = p.size < len ? p.size : (uint32_t)len;
        memcpy(buf, (void*)p.ptr, tocpy);

        if (src_ip)   *src_ip   = ip;
        if (src_port) *src_port = pt;

        this->remoteIP   = ip;
        this->remotePort = pt;

        free((void*)p.ptr, p.size);
        return tocpy;
    }

    int32_t close() override {
    while (r_head != r_tail) {
        free((void*)ring[r_head].ptr,
             ring[r_head].size);
        r_head = (r_head + 1) % UDP_RING_CAP;
    }
    udp_unbind(localPort, pid);
    s_by_port[localPort] = nullptr;
    bound = connected = false;
    return Socket::close();
}
};

UDPSocket* UDPSocket::s_by_port[MAX_PORTS] = { nullptr };
