#pragma once

#include "types.h"
#include "net/network_types.h"
#include "networking/port_manager.h"
#include "tcp.h"
#include "udp.h"

#ifdef __cplusplus
extern "C" {
#endif

//protos
#define PROTO_TCP 1
#define PROTO_UDP 2

//roles
#define SOCK_ROLE_CLIENT 0
#define SOCK_ROLE_SERVER 1

#define SOCK_OK              0
#define SOCK_ERR_INVAL      -1
#define SOCK_ERR_BOUND      -2
#define SOCK_ERR_NOT_BOUND  -3
#define SOCK_ERR_PERM       -4
#define SOCK_ERR_NO_PORT    -5
#define SOCK_ERR_SYS        -6
#define SOCK_ERR_PROTO      -7
#define SOCK_ERR_STATE      -8

#ifdef __cplusplus
}
#endif


#ifdef __cplusplus

class Socket {
protected:
    uint16_t    localPort   = 0;
    uint32_t    remoteIP    = 0;
    uint16_t    remotePort  = 0;
    uint8_t     proto;
    uint8_t     role;
    bool        bound       = false;
    bool        connected   = false;
    uint16_t    pid         = 0;

    Socket(uint8_t protocol, uint8_t r)
      : proto(protocol), role(r) {}

public:
    virtual ~Socket() { close(); }

    virtual int32_t bind(uint16_t port) = 0;

    virtual int32_t close() {
        if (bound) {
            if (proto == PROTO_UDP) {
                udp_unbind(localPort, pid);
            } else if (proto == PROTO_TCP) {
                tcp_unbind(localPort, pid);
            }
            bound     = false;
            localPort = 0;
        }
        connected = false;
        return SOCK_OK;
    }

    uint16_t get_local_port() const { return localPort; }
    uint16_t get_remote_port() const { return remotePort; }
    uint32_t get_remote_ip() const { return remoteIP; }
    uint8_t get_protocol() const { return proto; }
    uint8_t get_role() const { return role; }
    bool is_bound() const { return bound; }
    bool is_connected() const { return connected; }
};

#endif
