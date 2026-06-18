#include "csocket_tcp.h"
#include "networking/transport_layer/socket_bind.h"
#include "networking/transport_layer/tcp.h"
#include "networking/transport_layer/tcp/tcp_internal.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv4_route.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_route.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/net_logger/net_logger.h"
#include "syscalls/syscalls.h"
#include "std/memory.h"
#include "alloc/allocate.h"

#define TCP_MAX_BACKLOG 128
#define TCP_DEFAULT_SOCKET_BUF (256 * 1024)

typedef struct tcp_socket {
    uint16_t localPort;
    net_l4_endpoint remoteEP;
    bool connected;
    ksocket_t* ownerSocket;
    SocketOptions options;
    SockBindSpec bindSpec;
    socket_bind_token_t bindToken;
    tcp_data flow;
    ksocket_t** pending;
    int backlogCap;
    int backlogLen;
    bool listening;
} tcp_socket_t;

static ksocket_t* tcp_socket_pop_pending_at(tcp_socket_t* s, int idx) {
    if (!s || idx < 0 || idx >= s->backlogLen) return NULL;

    ksocket_t* client = s->pending[idx];
    for (int i = idx + 1; i < s->backlogLen; ++i) s->pending[i-1] = s->pending[i];

    s->pending[--s->backlogLen] = NULL;
    return client;
}

socket_impl_t socket_tcp_create(ksocket_t* owner, const SocketOptions* extra) {
    if (!owner) return NULL;

    tcp_socket_t* s = (tcp_socket_t*)zalloc(sizeof(*s));
    if (!s) return NULL;

    s->ownerSocket = owner;
    s->remoteEP.ver = IP_VER4;
    if (extra) s->options = *extra;
    if (!(s->options.flags & SOCK_OPT_BUF_SIZE)) {
        s->options.flags |= SOCK_OPT_BUF_SIZE;
        s->options.buf_size = TCP_DEFAULT_SOCKET_BUF;
    }

    if (!s->options.buf_size) s->options.buf_size = TCP_DEFAULT_SOCKET_BUF;
    if (s->options.buf_size > TCP_DEFAULT_SOCKET_BUF) s->options.buf_size = TCP_DEFAULT_SOCKET_BUF;
    return s;
}

int32_t socket_setopt_tcp(socket_impl_t sh, int32_t opt, const void* value, uint32_t len) {
    tcp_socket_t* s = (tcp_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;

    switch ((uint32_t)opt) {
        case SOCK_OPT_KEEPALIVE: {
            if (!value || len != sizeof(uint32_t)) return SOCK_ERR_INVAL;
            uint32_t v = 0;
            memcpy(&v, value, sizeof(v));
            if (v) {
                s->options.flags |= SOCK_OPT_KEEPALIVE;
                if (!s->options.keepalive_ms) s->options.keepalive_ms = SOCKET_DEFAULT_KEEPALIVE_MS;
            } else s->options.flags &= ~SOCK_OPT_KEEPALIVE;
            if (s->flow.flow_generation) tcp_flow_apply_socket_options(&s->flow, &s->options);
            return SOCK_OK;
        }
        case SOCK_OPT_KEEPALIVE_INTERVAL: {
            if (!value || len != sizeof(uint32_t)) return SOCK_ERR_INVAL;
            uint32_t v = 0;
            memcpy(&v, value, sizeof(v));
            if (!v) return SOCK_ERR_INVAL;
            s->options.keepalive_ms = v;
            s->options.flags |= SOCK_OPT_KEEPALIVE | SOCK_OPT_KEEPALIVE_INTERVAL;
            if (s->flow.flow_generation) tcp_flow_apply_socket_options(&s->flow, &s->options);
            return SOCK_OK;
        }
        case SOCK_OPT_TCP_NO_DELAY: {
            if (!value || len != sizeof(uint32_t)) return SOCK_ERR_INVAL;
            uint32_t v = 0;
            memcpy(&v, value, sizeof(v));
            if (v) s->options.flags |= SOCK_OPT_TCP_NO_DELAY;
            else s->options.flags &= ~SOCK_OPT_TCP_NO_DELAY;
            if (s->flow.flow_generation) tcp_flow_apply_socket_options(&s->flow, &s->options);
            return SOCK_OK;
        }
        case SOCK_OPT_SEND_BUF_SIZE: {
            if (!value || len != sizeof(uint32_t)) return SOCK_ERR_INVAL;
            uint32_t v = 0;
            memcpy(&v, value, sizeof(v));
            if (!v) return SOCK_ERR_INVAL;
            s->options.send_buf_size = v;
            s->options.flags |= SOCK_OPT_SEND_BUF_SIZE;
            if (s->flow.flow_generation) tcp_flow_apply_socket_options(&s->flow, &s->options);
            return SOCK_OK;
        }
        case SOCK_OPT_MCAST_JOIN:
        case SOCK_OPT_MCAST_LEAVE:
        case SOCK_OPT_BROADCAST_ALLOWED:
            return SOCK_ERR_INVAL;
        case SOCK_OPT_RECV_TIMEOUT:
        case SOCK_OPT_SEND_TIMEOUT:
        case SOCK_OPT_BUF_SIZE:
        case SOCK_OPT_DEBUG:
        case SOCK_OPT_DONTFRAG:
        case SOCK_OPT_TTL: {
            int32_t rc = socket_common_options_set(&s->options, opt, value, len);
            if (rc != SOCK_OK) return rc;
            if (s->flow.flow_generation) tcp_flow_apply_socket_options(&s->flow, &s->options);
            return SOCK_OK;
        }
        default:
            return SOCK_ERR_INVAL;
    }
}

int32_t socket_getopt_tcp(socket_impl_t sh, int32_t opt, void* value, uint32_t* len) {
    tcp_socket_t* s = (tcp_socket_t*)sh;
    if (!s || !len) return SOCK_ERR_INVAL;

    switch ((uint32_t)opt) {
        case SOCK_GET_REMOTE_ENDPOINT:
            if (!value) {
                *len = sizeof(net_l4_endpoint);
                return SOCK_OK;
            }
            if (*len < sizeof(net_l4_endpoint)) return SOCK_ERR_INVAL;
            memcpy(value, &s->remoteEP, sizeof(s->remoteEP));
            *len = sizeof(net_l4_endpoint);
            return SOCK_OK;
        case SOCK_GET_BIND_SPEC:
            if (!value) {
                *len = sizeof(SockBindSpec);
                return SOCK_OK;
            }
            if (*len < sizeof(SockBindSpec)) return SOCK_ERR_INVAL;
            memcpy(value, &s->bindSpec, sizeof(s->bindSpec));
            *len = sizeof(SockBindSpec);
            return SOCK_OK;
        default:
            break;
    }

    uint32_t v = 0;
    switch ((uint32_t)opt) {
        case SOCK_GET_BOUND:
            v = s->localPort != 0;
            break;
        case SOCK_GET_CONNECTED:
            v = s->connected;
            break;
        case SOCK_GET_LISTENING:
            v = s->listening;
            break;
        case SOCK_GET_LOCAL_PORT:
            v = s->localPort;
            break;
        case SOCK_GET_RECV_QUEUED:
            v = s->flow.flow_generation ? tcp_flow_readable(&s->flow) : 0;
            break;
        case SOCK_GET_SEND_QUEUED: {
            tcp_flow_t* flow = tcp_flow_from_ctx(&s->flow);
            if (flow) {
                v = flow->tx.queued_bytes + flow->tx.nagle_len;
                tcp_flow_put(flow);
            }
            break;
        }
        case SOCK_GET_TCP_STATE: {
            tcp_flow_t* flow = tcp_flow_from_ctx(&s->flow);
            if (flow) {
                v = flow->base.state;
                tcp_flow_put(flow);
            } else v = s->listening ? TCP_LISTEN : TCP_STATE_CLOSED;
            break;
        }
        case SOCK_GET_TCP_MSS: {
            tcp_flow_t* flow = tcp_flow_from_ctx(&s->flow);
            if (flow) {
                v = flow->tx.mss;
                tcp_flow_put(flow);
            }
            break;
        }
        case SOCK_GET_TCP_RTT_MS: {
            tcp_flow_t* flow = tcp_flow_from_ctx(&s->flow);
            if (flow) {
                v = flow->tx.rtt_valid ? flow->tx.srtt : 0;
                tcp_flow_put(flow);
            }
            break;
        }
        case SOCK_GET_TCP_RETRANSMITS: {
            tcp_flow_t* flow = tcp_flow_from_ctx(&s->flow);
            if (flow) {
                for (int i = 0; i < TCP_MAX_TX_SEGS; ++i) if (flow->tx.txq[i].used) v += flow->tx.txq[i].retransmit_cnt;
                tcp_flow_put(flow);
            }
            break;
        }
        case SOCK_GET_OPT_KEEPALIVE:
            v = (s->options.flags & SOCK_OPT_KEEPALIVE) != 0;
            break;
        case SOCK_GET_OPT_KEEPALIVE_INTERVAL:
            v = s->options.keepalive_ms;
            break;
        case SOCK_GET_OPT_TCP_NO_DELAY:
            v = (s->options.flags & SOCK_OPT_TCP_NO_DELAY) != 0;
            break;
        case SOCK_GET_OPT_SEND_BUF_SIZE:
            v = s->options.send_buf_size;
            break;
        case SOCK_GET_MCAST_GROUPS:
        case SOCK_GET_OPT_BROADCAST_ALLOWED:
            return SOCK_ERR_INVAL;
        case SOCK_GET_OPT_RECV_TIMEOUT:
        case SOCK_GET_OPT_SEND_TIMEOUT:
        case SOCK_GET_OPT_BUF_SIZE:
        case SOCK_GET_OPT_DEBUG:
        case SOCK_GET_OPT_DONTFRAG:
        case SOCK_GET_OPT_TTL:
            return socket_common_options_get(&s->options, opt, value, len);
        default:
            return SOCK_ERR_INVAL;
    }

    if (!value) {
        *len = sizeof(uint32_t);
        return SOCK_OK;
    }
    if (*len < sizeof(uint32_t)) return SOCK_ERR_INVAL;
    memcpy(value, &v, sizeof(v));
    *len = sizeof(uint32_t);
    return SOCK_OK;
}

int32_t socket_bind_tcp(socket_impl_t sh, const SockBindSpec* spec_in, uint16_t port) {
    tcp_socket_t* s = (tcp_socket_t*)sh;
    if (!s || !spec_in) return SOCK_ERR_INVAL;

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_TCP;
    ev.action = NETLOG_ACT_BIND;
    ev.pid = socket_core_pid(s->ownerSocket);
    ev.u0 = port;
    ev.bind_spec = *spec_in;
    netlog_socket_event(&s->options, &ev);

    if (s->localPort) return SOCK_ERR_BOUND;
    if (!s->ownerSocket) return SOCK_ERR_SYS;

    SockBindSpec spec = *spec_in;
    if (!socket_bind_normalize_spec(&spec)) return SOCK_ERR_INVAL;

    if (spec.kind == BIND_L3) {
        if (!spec.l3_id) return SOCK_ERR_INVAL;
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(spec.l3_id);
        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(spec.l3_id);
        bool ok4 = (spec.ver == 0 || spec.ver == IP_VER4) && ipv4_l3_is_ready(v4);
        bool ok6 = (spec.ver == 0 || spec.ver == IP_VER6) && ipv6_l3_is_tcp_usable(v6);
        if (!ok4 && !ok6) return SOCK_ERR_INVAL;
    } else if (spec.kind == BIND_L2) {
        l2_interface_t* l2 = l2_interface_find_by_index(spec.ifindex);
        if (!l2 || !l2->is_up) return SOCK_ERR_INVAL;
    } else if (spec.kind == BIND_IP) {
        if (spec.ver == IP_VER4) {
            uint32_t v4ip = 0;
            memcpy(&v4ip, spec.ip, 4);
            if (!ipv4_is_unspecified(v4ip) && !ipv4_l3_is_ready(l3_ipv4_find_by_ip(v4ip))) return SOCK_ERR_INVAL;
        } else if (spec.ver == IP_VER6) {
            if (!ipv6_is_unspecified(spec.ip) && !ipv6_l3_is_tcp_usable(l3_ipv6_find_by_ip(spec.ip))) return SOCK_ERR_INVAL;
        } else return SOCK_ERR_INVAL;
    } else if (spec.kind != BIND_ANY && spec.kind != BIND_ANY4 && spec.kind != BIND_ANY6) return SOCK_ERR_INVAL;

    int bind_port = port;
    socket_bind_token_t token = 0;
    if (bind_port == 0) {
        bind_port = socket_bind_alloc_ephemeral(s->ownerSocket, PROTO_TCP, &spec, &token);
        if (bind_port < 0) return SOCK_ERR_NO_PORT;
    } else if (!socket_bind_insert(s->ownerSocket, PROTO_TCP, &spec, port, &token)) return SOCK_ERR_BOUND;

    s->bindSpec = spec;
    s->bindToken = token;
    s->localPort = (uint16_t)bind_port;
    return SOCK_OK;
}

int32_t socket_listen_tcp(socket_impl_t sh, int32_t backlog) {
    tcp_socket_t* s = (tcp_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;
    if (!s->bindToken || s->connected) return SOCK_ERR_STATE;

    int cap = backlog > TCP_MAX_BACKLOG ? TCP_MAX_BACKLOG : backlog;
    if (cap < 1) cap = 1;

    if (s->pending) {
        for (int i = 0; i < s->backlogLen; ++i) {
            if (!s->pending[i]) continue;
            socket_core_close_socket(s->pending[i]);
            socket_core_put(s->pending[i]);
        }
        release(s->pending);
    }

    s->pending = (ksocket_t**)zalloc(sizeof(ksocket_t*) * cap);
    if (!s->pending) {
        s->backlogCap = 0;
        s->backlogLen = 0;
        s->listening = false;
        return SOCK_ERR_SYS;
    }

    s->backlogCap = cap;
    s->backlogLen = 0;
    s->listening = true;
    return SOCK_OK;
}

//TODO replace polling with a socket wait queue when kernel events are present
ksocket_t* socket_accept_tcp(socket_impl_t sh) {
    tcp_socket_t* s = (tcp_socket_t*)sh;
    if (!s || !s->listening || !s->pending) return NULL;

    const int max_empty_iters = 200;
    const int ready_wait_iters = 4;
    int iter = 0;
    while (s->backlogLen == 0) {
        if (++iter > max_empty_iters) return NULL;
        msleep(5);
    }

    iter = 0;
    while (1) {
        for (int i = 0; i < s->backlogLen;) {
            ksocket_t* owner = s->pending[i];
            tcp_socket_t* client = owner ? (tcp_socket_t*)socket_core_impl(owner) : NULL;

            if (client && client->flow.flow_generation) {
                bool readable = tcp_flow_readable(&client->flow) != 0;
                bool closed = tcp_flow_recv_closed(&client->flow);
                if (!closed || readable) {
                    ++i;
                    continue;
                }
            }

            owner = tcp_socket_pop_pending_at(s, i);
            if (owner) {
                socket_core_close_socket(owner);
                socket_core_put(owner);
            }
        }

        if (s->backlogLen == 0) return NULL;

        for (int i = 0; i < s->backlogLen; ++i) {
            ksocket_t* owner = s->pending[i];
            tcp_socket_t* client = owner ? (tcp_socket_t*)socket_core_impl(owner) : NULL;
            if (!client) continue;
            if (!client->flow.flow_generation || tcp_flow_readable(&client->flow) || tcp_flow_recv_closed(&client->flow)) return tcp_socket_pop_pending_at(s, i);
        }

        if (++iter > ready_wait_iters) {
            if (s->backlogLen >= s->backlogCap) {
                ksocket_t* owner = tcp_socket_pop_pending_at(s, 0);
                if (owner) {
                    socket_core_close_socket(owner);
                    socket_core_put(owner);
                }
            }
            return NULL;
        }
        msleep(5);
    }
}

int32_t socket_connect_tcp(socket_impl_t sh, const net_l4_endpoint* dst) {
    tcp_socket_t* s = (tcp_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_TCP;
    ev.action = NETLOG_ACT_CONNECT;
    ev.pid = socket_core_pid(s->ownerSocket);
    if (dst) ev.dst_ep = *dst;
    netlog_socket_event(&s->options, &ev);

    if (s->connected || s->listening) return SOCK_ERR_STATE;
    if (!dst || !dst->port) return SOCK_ERR_INVAL;

    net_l4_endpoint d = *dst;
    uint8_t chosen_l3 = 0;

    if (d.ver == IP_VER6) {
        ipv6_tx_plan_t p6;
        if (!socket_bind_build_ipv6_tx_plan(&s->bindSpec, s->localPort, d.ip, &p6)) return SOCK_ERR_SYS;
        chosen_l3 = p6.l3_id;
    } else if (d.ver == IP_VER4) {
        uint32_t dip = 0;
        memcpy(&dip, d.ip, 4);
        ipv4_tx_plan_t p4;
        if (!socket_bind_build_ipv4_tx_plan(&s->bindSpec, s->localPort, dip, &p4)) return SOCK_ERR_SYS;
        chosen_l3 = p4.l3_id;
    } else return SOCK_ERR_INVAL;

    if (!chosen_l3) return SOCK_ERR_SYS;

    if (d.ver == IP_VER4) {
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
        if (!ipv4_l3_is_ready(v4)) return SOCK_ERR_SYS;
    } else if (d.ver == IP_VER6) {
        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(chosen_l3);
        if (!ipv6_l3_is_tcp_usable(v6)) return SOCK_ERR_SYS;
    } else return SOCK_ERR_INVAL;

    bool ephemeral_allocated = false;
    if (s->localPort == 0) {
        if (!s->ownerSocket) return SOCK_ERR_SYS;
        socket_bind_token_t token = 0;
        int p = socket_bind_alloc_ephemeral_l3(s->ownerSocket, PROTO_TCP, chosen_l3, &token);
        if (p < 0) return SOCK_ERR_NO_PORT;
        s->localPort = (uint16_t)p;
        s->bindToken = token;
        memset(&s->bindSpec, 0, sizeof(s->bindSpec));
        s->bindSpec.kind = BIND_L3;
        s->bindSpec.ver = d.ver;
        s->bindSpec.l3_id = chosen_l3;
        ephemeral_allocated = true;
    }

    memset(&s->flow, 0, sizeof(s->flow));
    bool ok = tcp_handshake_l3(chosen_l3, s->localPort, &d, &s->flow, &s->options);
    if (ok && d.ver == IP_VER4) {
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
        ok = ipv4_l3_is_ready(v4);
    } else if (ok && d.ver == IP_VER6) ok = ipv6_l3_is_tcp_usable(l3_ipv6_find_by_id(chosen_l3));

    if (!ok) {
        if (ephemeral_allocated) {
            if (s->bindToken) {
                socket_bind_remove(s->bindToken);
                s->bindToken = 0;
            }
            s->localPort = 0;
            memset(&s->bindSpec, 0, sizeof(s->bindSpec));
            s->connected = false;
            s->remoteEP.port = 0;
            s->remoteEP.ver = IP_VER4;
            memset(s->remoteEP.ip, 0, sizeof(s->remoteEP.ip));
        }
        return SOCK_ERR_SYS;
    }

    s->remoteEP = d;
    s->connected = true;

    netlog_socket_event_t ev1 = {0};
    ev1.comp = NETLOG_COMP_TCP;
    ev1.action = NETLOG_ACT_CONNECTED;
    ev1.pid = socket_core_pid(s->ownerSocket);
    ev1.u0 = s->localPort;
    ev1.u1 = s->remoteEP.port;
    ev1.local_port = s->localPort;
    ev1.remote_ep = s->remoteEP;
    netlog_socket_event(&s->options, &ev1);
    return SOCK_OK;
}

int64_t socket_send_tcp(socket_impl_t sh, const void* buf, uint64_t len) {
    tcp_socket_t* s = (tcp_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_TCP;
    ev.action = NETLOG_ACT_SEND;
    ev.pid = socket_core_pid(s->ownerSocket);
    ev.u0 = (uint32_t)len;
    ev.local_port = s->localPort;
    ev.remote_ep = s->remoteEP;
    netlog_socket_event(&s->options, &ev);

    if (!s->connected || !s->flow.flow_generation) return SOCK_ERR_STATE;
    if (!buf && len) return SOCK_ERR_INVAL;
    if (!len) return 0;

    uint32_t chunk = len > UINT32_MAX ? UINT32_MAX : (uint32_t)len;
    uint32_t start_ms = (uint32_t)get_time();
    while (1) {
        s->flow.payload.ptr = (uintptr_t)buf;
        s->flow.payload.size = chunk;
        s->flow.flags = (1u << PSH_F) | (1u << ACK_F);

        tcp_result_t res = tcp_flow_send(&s->flow);
        if (res != TCP_OK && res != TCP_WOULDBLOCK) {
            switch (res) {
                case TCP_BUSY:
                case TCP_TIMEOUT:
                    return SOCK_ERR_WOULDBLOCK;
                case TCP_UNIMPLEMENT:
                    return SOCK_ERR_UNSUP;
                case TCP_INVALID:
                case TCP_RESET:
                case TCP_DISCONNECT:
                    return SOCK_ERR_STATE;
                case TCP_CSUM_ERR:
                    return SOCK_ERR_PROTO;
                default:
                    return SOCK_ERR_SYS;
            }
        }
        if (s->flow.payload.size) return (int64_t)s->flow.payload.size;
        if (!(s->options.flags & SOCK_OPT_SEND_TIMEOUT) || !s->options.send_timeout_ms) return SOCK_ERR_WOULDBLOCK;

        uint32_t now_ms = (uint32_t)get_time();
        uint32_t elapsed_ms = now_ms - start_ms;
        if (elapsed_ms >= s->options.send_timeout_ms) return SOCK_ERR_WOULDBLOCK;
        uint32_t wait_ms = s->options.send_timeout_ms - elapsed_ms;
        if (wait_ms > 5) wait_ms = 5;
        msleep(wait_ms);
    }
}

int64_t socket_recv_tcp(socket_impl_t sh, void* buf, uint64_t len) {
    tcp_socket_t* s = (tcp_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_TCP;
    ev.action = NETLOG_ACT_RECV;
    ev.pid = socket_core_pid(s->ownerSocket);
    ev.u0 = (uint32_t)len;
    ev.local_port = s->localPort;
    ev.remote_ep = s->remoteEP;
    netlog_socket_event(&s->options, &ev);

    if (!buf || !len) return 0;
    if (!s->connected || !s->flow.flow_generation) return s->connected ? SOCK_ERR_WOULDBLOCK : 0;

    uint32_t start_ms = (uint32_t)get_time();
    while (1) {
        int64_t n = tcp_flow_read(&s->flow, buf, len);
        if (n > 0) return n;
        if (n == TCP_DISCONNECT) return 0;
        if (n < 0) return SOCK_ERR_STATE;
        if (tcp_flow_recv_closed(&s->flow)) return 0;
        if (!s->connected) return 0;
        if (!(s->options.flags & SOCK_OPT_RECV_TIMEOUT) || !s->options.recv_timeout_ms) return SOCK_ERR_WOULDBLOCK;

        uint32_t now_ms = (uint32_t)get_time();
        uint32_t elapsed_ms = now_ms - start_ms;
        if (elapsed_ms >= s->options.recv_timeout_ms) return SOCK_ERR_WOULDBLOCK;

        uint32_t wait_ms = s->options.recv_timeout_ms - elapsed_ms;
        if (wait_ms > 5) wait_ms = 5;
        msleep(wait_ms);
    }
}

int32_t socket_close_tcp(socket_impl_t sh) {
    tcp_socket_t* s = (tcp_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_TCP;
    ev.action = NETLOG_ACT_CLOSE;
    ev.pid = socket_core_pid(s->ownerSocket);
    ev.local_port = s->localPort;
    ev.remote_ep = s->remoteEP;
    netlog_socket_event(&s->options, &ev);

    if (s->connected && s->flow.flow_generation) {
        if (!tcp_flow_is_closed(&s->flow)) {
            tcp_flow_flush(&s->flow);
            tcp_flow_close(&s->flow);
        }
        s->connected = false;
        memset(&s->flow, 0, sizeof(s->flow));
    }

    if (s->pending) {
        for (int i = 0; i < s->backlogLen; ++i) {
            if (!s->pending[i]) continue;
            socket_core_close_socket(s->pending[i]);
            socket_core_put(s->pending[i]);
            s->pending[i] = NULL;
        }
        release(s->pending);
        s->pending = NULL;
    }
    s->backlogCap = 0;
    s->backlogLen = 0;
    s->listening = false;

    if (s->bindToken) {
        socket_bind_remove(s->bindToken);
        s->bindToken = 0;
    }
    s->localPort = 0;
    memset(&s->bindSpec, 0, sizeof(s->bindSpec));
    s->connected = false;
    s->remoteEP.port = 0;
    s->remoteEP.ver = IP_VER4;
    memset(s->remoteEP.ip, 0, sizeof(s->remoteEP.ip));
    return SOCK_OK;
}

void socket_destroy_tcp(socket_impl_t sh) {
    tcp_socket_t* s = (tcp_socket_t*)sh;
    if (!s) return;
    socket_close_tcp(s);
    release(s);
}

const SocketOptions* socket_tcp_options(socket_impl_t sh) {
    tcp_socket_t* s = (tcp_socket_t*)sh;
    if (!s) return NULL;
    return &s->options;
}

uint32_t tcp_accept_enqueue(ksocket_t* listener, ip_version_t ipver, const void* src_ip_addr, const void* dst_ip_addr, uint16_t src_port, uint16_t dst_port) {
    if (!listener) return 0;
    tcp_socket_t* s = (tcp_socket_t*)socket_core_impl(listener);
    if (!s) return 0;
    if (!s->listening || !s->localPort || s->localPort != dst_port || !s->pending) return 0;

    for (int i = 0; i < s->backlogLen;) {
        ksocket_t* owner = s->pending[i];
        tcp_socket_t* client = owner ? (tcp_socket_t*)socket_core_impl(owner) : NULL;
        if (client && client->flow.flow_generation) {
            bool readable = tcp_flow_readable(&client->flow) != 0;
            bool closed = tcp_flow_recv_closed(&client->flow);
            if (!closed || readable) {
                ++i;
                continue;
            }
        }

        owner = tcp_socket_pop_pending_at(s, i);
        if (owner) {
            socket_core_close_socket(owner);
            socket_core_put(owner);
        }
    }
    if (s->backlogLen >= s->backlogCap) return 0;

    ksocket_t* child_owner = NULL;
    uint16_t owner_pid = socket_core_pid(s->ownerSocket);
    if (!socket_core_alloc(PROTO_TCP, owner_pid, &child_owner)) return 0;

    tcp_socket_t* child = (tcp_socket_t*)socket_tcp_create(child_owner, &s->options);
    if (!child) {
        socket_core_close_socket(child_owner);
        return 0;
    }

    child->localPort = dst_port;
    child->remoteEP.ver = ipver;
    memset(child->remoteEP.ip, 0, sizeof(child->remoteEP.ip));
    if (ipver == IP_VER4) memcpy(child->remoteEP.ip, src_ip_addr, 4);
    else ipv6_cpy(child->remoteEP.ip, src_ip_addr);
    child->remoteEP.port = src_port;
    child->bindSpec.kind = BIND_IP;
    child->bindSpec.ver = ipver;
    if (ipver == IP_VER4) memcpy(child->bindSpec.ip, dst_ip_addr, 4);
    else if (ipver == IP_VER6) ipv6_cpy(child->bindSpec.ip, dst_ip_addr);

    if (!tcp_get_ctx(dst_port, ipver, dst_ip_addr, child->remoteEP.ip, src_port, &child->flow)) {
        socket_destroy_tcp(child);
        socket_core_close_socket(child_owner);
        return 0;
    }

    if (!socket_core_attach_impl(child_owner, child, socket_destroy_tcp, socket_close_tcp, socket_setopt_tcp, socket_getopt_tcp)) {
        socket_destroy_tcp(child);
        socket_core_close_socket(child_owner);
        return 0;
    }

    child->connected = true;
    socket_core_ref(child_owner);
    s->pending[s->backlogLen++] = child_owner;
    return 1;
}
