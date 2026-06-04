#pragma once
#include "types.h"
#include "net/network_types.h"
#include "networking/transport_layer/socket_core.h"
#include "networking/transport_layer/socket_bind.h"
#include "tcp.h"
#include "udp.h"
#include "net/socket_types.h"
#include "console/kio.h"
#include "networking/net_logger/net_logger.h"

#ifdef __cplusplus

//TODO should these sockets be c instead of cpp?
class Socket {
protected:
    uint16_t localPort = 0;
    net_l4_endpoint remoteEP = { IP_VER4, {0}, 0 };

    uint8_t proto = 0;
    uint8_t role = 0;
    bool bound = false;
    bool connected = false;
    uint16_t pid = 0;
    ksocket_t* ownerSocket = nullptr;

    SocketExtraOptions extraOpts = {};
    SockBindSpec bindSpec = {};


    Socket(ksocket_t* owner, uint8_t protocol, uint8_t r, const SocketExtraOptions* extra) : proto(protocol), role(r), ownerSocket(owner) {
        if (extra) extraOpts = *extra;
    }


public:
    virtual ~Socket() { close(); }

    ksocket_t* get_owner_socket() const { return ownerSocket; }
    const SocketExtraOptions* get_extra_options() const { return &extraOpts; }

    int32_t set_option(int32_t opt, const void* value, uint32_t len) {
        if (!value || len != sizeof(uint32_t)) return SOCK_ERR_INVAL;

        uint32_t v = 0;
        memcpy(&v, value, sizeof(v));

        switch ((uint32_t)opt) {
            case SOCK_OPT_RECV_TIMEOUT:
                extraOpts.recv_timeout_ms = v;
                if (v) extraOpts.flags |= SOCK_OPT_RECV_TIMEOUT;
                else extraOpts.flags &= ~SOCK_OPT_RECV_TIMEOUT;
                return SOCK_OK;
            case SOCK_OPT_SEND_TIMEOUT:
                extraOpts.send_timeout_ms = v;
                if (v) extraOpts.flags |= SOCK_OPT_SEND_TIMEOUT;
                else extraOpts.flags &= ~SOCK_OPT_SEND_TIMEOUT;
                return SOCK_OK;
            case SOCK_OPT_BUF_SIZE:
                if (!v) return SOCK_ERR_INVAL;
                extraOpts.buf_size = v;
                extraOpts.flags |= SOCK_OPT_BUF_SIZE;
                return SOCK_OK;
            case SOCK_OPT_DEBUG:
                if (v > SOCK_DBG_ALL) return SOCK_ERR_INVAL;
                extraOpts.debug_level = (SockDebugLevel)v;
                if (v) extraOpts.flags |= SOCK_OPT_DEBUG;
                else extraOpts.flags &= ~SOCK_OPT_DEBUG;
                return SOCK_OK;
            case SOCK_OPT_DONTFRAG:
                if (v) extraOpts.flags |= SOCK_OPT_DONTFRAG;
                else extraOpts.flags &= ~SOCK_OPT_DONTFRAG;
                return SOCK_OK;
            case SOCK_OPT_TTL:
                if (v > 255) return SOCK_ERR_INVAL;
                extraOpts.ttl = (uint8_t)v;
                if (v) extraOpts.flags |= SOCK_OPT_TTL;
                else extraOpts.flags &= ~SOCK_OPT_TTL;
                return SOCK_OK;
            default:
                return SOCK_ERR_INVAL;
        }
    }

    int32_t get_option(int32_t opt, void* value, uint32_t* len) const {
        if (!value || !len || *len < sizeof(uint32_t)) return SOCK_ERR_INVAL;

        uint32_t v = 0;
        switch ((uint32_t)opt) {
            case SOCK_OPT_RECV_TIMEOUT:
                v = extraOpts.recv_timeout_ms;
                break;
            case SOCK_OPT_SEND_TIMEOUT:
                v = extraOpts.send_timeout_ms;
                break;
            case SOCK_OPT_BUF_SIZE:
                v = extraOpts.buf_size;
                break;
            case SOCK_OPT_DEBUG:
                v = extraOpts.debug_level;
                break;
            case SOCK_OPT_DONTFRAG:
                v = (extraOpts.flags & SOCK_OPT_DONTFRAG) != 0;
                break;
            case SOCK_OPT_TTL:
                v = extraOpts.ttl;
                break;
            default:
                return SOCK_ERR_INVAL;
        }

        memcpy(value, &v, sizeof(v));
        *len = sizeof(uint32_t);
        return SOCK_OK;
    }

    virtual int32_t bind(const SockBindSpec& spec, uint16_t port) = 0;

    virtual int32_t close() {
        if (ownerSocket) socket_bind_remove_socket(ownerSocket);
        bound = false;
        localPort = 0;
        connected = false;
        remoteEP.port = 0;
        remoteEP.ver = IP_VER4;
        memset(remoteEP.ip, 0, 16);
        memset(&bindSpec, 0, sizeof(bindSpec));
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

};

#endif
