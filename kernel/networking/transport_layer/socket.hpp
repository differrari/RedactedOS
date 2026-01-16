#pragma once
#include "types.h"
#include "net/network_types.h"
#include "networking/port_manager.h"
#include "tcp.h"
#include "udp.h"
#include "net/socket_types.h"
#include "console/kio.h"
#include "networking/net_logger/net_logger.h"

#ifdef __cplusplus
extern "C" {
#endif

//TODO: replace with enum
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
#define SOCK_ERR_DNS        -9

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

class Socket {
protected:
    static constexpr int SOCK_MAX_L3 = 32;

    uint16_t localPort = 0;
    net_l4_endpoint remoteEP = { IP_VER4, {0}, 0 };

    uint8_t proto = 0;
    uint8_t role = 0;
    bool bound = false;
    bool connected = false;
    uint16_t pid = 0;

    SocketExtraOptions extraOpts = {};

    uint8_t bound_l3[SOCK_MAX_L3] = {0};
    int bound_l3_count = 0;

    Socket(uint8_t protocol, uint8_t r, const SocketExtraOptions* extra) : proto(protocol), role(r) {
        if (extra) extraOpts = *extra;
    }

    virtual void do_unbind_one(uint8_t l3_id, uint16_t port, uint16_t pid) = 0;

    void clear_bound_l3() { bound_l3_count = 0; }
    bool add_bound_l3(uint8_t l3_id) { if (bound_l3_count >= SOCK_MAX_L3) return false; bound_l3[bound_l3_count++] = l3_id; return true; }
    void set_remote_endpoint(const net_l4_endpoint& ep) { remoteEP = ep; }

public:
    virtual ~Socket() { close(); }

    virtual int32_t bind(const SockBindSpec& spec, uint16_t port) = 0;

    virtual int32_t close() {
        if (bound) {
            for (int i = 0; i < bound_l3_count; ++i) {
                do_unbind_one(bound_l3[i], localPort, pid);
            }
            bound = false;
            localPort = 0;
            clear_bound_l3();
        }
        connected = false;
        remoteEP.port = 0;
        remoteEP.ver = IP_VER4;
        memset(remoteEP.ip, 0, 16);
        return SOCK_OK;
    }

    uint16_t get_local_port() const { return localPort; }
    uint16_t get_remote_port() const { return remoteEP.port; }
    uint8_t get_protocol() const { return proto; }
    uint8_t get_role() const { return role; }
    uint16_t get_pid() const { return pid; }
    bool is_bound() const { return bound; }
    bool is_connected() const { return connected; }

    ip_version_t get_remote_ip_version() const { return remoteEP.ver; }
    const uint8_t* get_remote_ip_bytes() const { return remoteEP.ip; }
    const net_l4_endpoint& get_remote_endpoint() const { return remoteEP; }

    int get_bound_l3_count() const { return bound_l3_count; }
    uint8_t get_bound_l3_id(int idx) const { return (idx >= 0 && idx < bound_l3_count) ? bound_l3[idx] : 0; }
};

#endif
