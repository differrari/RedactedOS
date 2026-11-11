#include "csocket_tcp.h"
#include "networking/transport_layer/socket_bind.h"
#include "networking/transport_layer/tcp.h"
#include "networking/transport_layer/tcp/tcp_internal.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/net_logger/net_logger.h"
#include "networking/firewall.h"
#include "syscalls/syscalls.h"
#include "std/memory.h"
#include "alloc/allocate.h"
#include "exceptions/irq.h"
#include "data/struct/ring_buffer.h"

#define TCP_MAX_BACKLOG 128

typedef struct tcp_socket {
    uint16_t localPort;
    net_l4_endpoint remoteEP;
    bool connected;
    ksocket_t* ownerSocket;
    SocketOptions options;
    SockBindSpec bindSpec;
    socket_bind_token_t bindToken;
    uint32_t bind_generation;
    tcp_data flow;
    CRingBuffer pending;
    bool listening;
    bool closed;
} tcp_socket_t;

static int32_t tcp_socket_backlog_len(tcp_socket_t* s) {
    if (!s) return 0;

    irq_flags_t irq = irq_save_disable();
    int32_t len = (int32_t)cring_size(&s->pending);
    irq_restore(irq);
    return len;
}

static ksocket_t* tcp_socket_pop_pending_at(tcp_socket_t* s, uint32_t idx, ksocket_t* expected) {
    if (!s) return NULL;

    irq_flags_t irq = irq_save_disable();
    ksocket_t** item = (ksocket_t**)cring_at(&s->pending, idx);
    if (!item || (expected && *item != expected)) {
        irq_restore(irq);
        return NULL;
    }

    ksocket_t* client = NULL;
    if (idx == 0) cring_pop(&s->pending, &client);
    else cring_remove(&s->pending, idx, &client);
    irq_restore(irq);
    return client;
}

static void tcp_socket_reset_connection(tcp_socket_t* s, bool abort_flow) {
    if (!s) return;
    if (abort_flow && s->flow.flow_generation) tcp_flow_abort(&s->flow);

    s->connected = false;
    memset(&s->flow, 0, sizeof(s->flow));
    make_ep(0, 0, IP_VER4, &s->remoteEP);
}

static void tcp_socket_abort_pending(ksocket_t* owner) {
    if (!owner) return;

    tcp_socket_t* child = (tcp_socket_t*)socket_core_impl(owner);
    if (child && child->flow.flow_generation) tcp_socket_reset_connection(child, true);

    socket_core_close_socket(owner);
    socket_core_put(owner);
}

static int32_t tcp_socket_connection_state(tcp_socket_t* s) { 
    if (!s) return -1;
    if (!s->flow.flow_generation) {
        tcp_socket_reset_connection(s, false);
        return -1;
    }

    tcp_flow_t* flow = tcp_flow_from_ctx(&s->flow);
    if (!flow) {
        tcp_socket_reset_connection(s, false);
        return -1;
    }

    tcp_state_t state = flow->base.state;
    tcp_flow_put(flow);

    if (state == TCP_ESTABLISHED || state == TCP_CLOSE_WAIT) {
        s->connected = true;
        return 1;
    }

    if (state == TCP_STATE_CLOSED || state == TCP_TIME_WAIT) {
        if (tcp_flow_release_closed(&s->flow) == TCP_BUSY) return 0;
        tcp_socket_reset_connection(s, false);
        return -1;
    }

    return 0;
}

socket_impl_t socket_tcp_create(ksocket_t* owner, const SocketOptions* extra) {
    if (!owner) return NULL;

    SockOptFlags supported = SOCK_OPT_KEEPALIVE | SOCK_OPT_SEND_TIMEOUT | SOCK_OPT_RECV_TIMEOUT | SOCK_OPT_BUF_SIZE | SOCK_OPT_DEBUG | SOCK_OPT_DONTFRAG | SOCK_OPT_TTL | SOCK_OPT_SEND_BUF_SIZE | SOCK_OPT_TCP_NO_DELAY | SOCK_OPT_NONBLOCK | SOCK_OPT_DONTROUTE | SOCK_OPT_REUSEADDR | SOCK_OPT_TCP_MAXSEG | SOCK_OPT_LINGER | SOCK_OPT_TCP_SACK | SOCK_OPT_TCP_DSACK | SOCK_OPT_TCP_KEEPIDLE | SOCK_OPT_TCP_KEEPINTVL | SOCK_OPT_TCP_KEEPCNT | SOCK_OPT_TCP_USER_TIMEOUT | SOCK_OPT_TCP_QUICKACK | SOCK_OPT_TCP_CORK | SOCK_OPT_TCP_SYNCNT;
    if (extra) {
        if (extra->flags & ~supported) return NULL;
        if ((extra->flags & SOCK_OPT_DEBUG) && extra->debug_level > SOCK_DBG_ALL) return NULL;
        if ((extra->flags & SOCK_OPT_BUF_SIZE) && !extra->buf_size) return NULL;
        if ((extra->flags & SOCK_OPT_SEND_BUF_SIZE) && !extra->send_buf_size) return NULL;
    }

    tcp_socket_t* s = (tcp_socket_t*)zalloc(sizeof(*s));
    if (!s) return NULL;

    s->ownerSocket = owner;
    s->remoteEP.ver = IP_VER4;
    s->bindSpec.kind = BIND_ANY;
    if (extra) s->options = *extra;
    if (!(s->options.flags & SOCK_OPT_TCP_KEEPIDLE)) s->options.tcp_keepidle = 0;
    if (!(s->options.flags & SOCK_OPT_TCP_KEEPINTVL)) s->options.tcp_keepintvl = 0;
    if (!(s->options.flags & SOCK_OPT_TCP_KEEPCNT)) s->options.tcp_keepcnt = 0;
    if (!(s->options.flags & SOCK_OPT_TCP_USER_TIMEOUT)) s->options.tcp_user_timeout_ms = 0;
    if (!(s->options.flags & SOCK_OPT_TCP_SYNCNT)) s->options.tcp_syncnt = 0;
    s->options.flags |= SOCK_OPT_TCP_SACK | SOCK_OPT_TCP_DSACK;
    s->options.flags |= SOCK_OPT_DONTFRAG;
    if (!(s->options.flags & SOCK_OPT_BUF_SIZE)) {
        s->options.flags |= SOCK_OPT_BUF_SIZE;
        s->options.buf_size = TCP_DEFAULT_RCV_BUF;
    }

    s->options.buf_size = tcp_clamp_rcvbuf(s->options.buf_size);
    if (!(s->options.flags & SOCK_OPT_TCP_MAXSEG) || !s->options.tcp_maxseg) {
        s->options.flags &= ~SOCK_OPT_TCP_MAXSEG;
        s->options.tcp_maxseg = 0;
    } else if (s->options.tcp_maxseg < 256u || s->options.tcp_maxseg > TCP_MAX_MSS) {
        release(s);
        return NULL;
    }
    if (!(s->options.flags & SOCK_OPT_LINGER) || !s->options.linger.enabled) {
        s->options.flags &= ~SOCK_OPT_LINGER;
        memset(&s->options.linger, 0, sizeof(s->options.linger));
    } else s->options.linger.enabled = 1;
    if (s->options.flags & SOCK_OPT_TCP_DSACK) s->options.flags |= SOCK_OPT_TCP_SACK;
    if (((s->options.flags & SOCK_OPT_TCP_KEEPIDLE) && (!s->options.tcp_keepidle || s->options.tcp_keepidle > UINT32_MAX / 1000)) || ((s->options.flags & SOCK_OPT_TCP_KEEPINTVL) && 
    (!s->options.tcp_keepintvl || s->options.tcp_keepintvl > UINT32_MAX / 1000)) || ((s->options.flags & SOCK_OPT_TCP_KEEPCNT) && (!s->options.tcp_keepcnt || s->options.tcp_keepcnt > 255))) {
        release(s);
        return NULL;
    }
    if ((s->options.flags & SOCK_OPT_TCP_SYNCNT) && (!s->options.tcp_syncnt || s->options.tcp_syncnt > 255)) {
        release(s);
        return NULL;
    }
    return s;
}

int32_t socket_setopt_tcp(socket_impl_t sh, SockOptFlags opt, const void* value, uint32_t len) {
    tcp_socket_t* s = (tcp_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;

    switch (opt) {
        case SOCK_OPT_KEEPALIVE: {
            if (!value || len != sizeof(uint32_t)) return SOCK_ERR_INVAL;
            uint32_t v = 0;
            memcpy(&v, value, sizeof(v));
            if (v) {
                s->options.flags |= SOCK_OPT_KEEPALIVE;
            } else s->options.flags &= ~SOCK_OPT_KEEPALIVE;
            if (s->flow.flow_generation) tcp_flow_apply_socket_options(&s->flow, &s->options, opt);
            return SOCK_OK;
        }
        case SOCK_OPT_TCP_KEEPIDLE:
        case SOCK_OPT_TCP_KEEPINTVL:
        case SOCK_OPT_TCP_KEEPCNT:
        case SOCK_OPT_TCP_USER_TIMEOUT:
        case SOCK_OPT_TCP_QUICKACK:
        case SOCK_OPT_TCP_CORK:
        case SOCK_OPT_TCP_SYNCNT: {
            if (!value || len != sizeof(uint32_t)) return SOCK_ERR_INVAL;
            uint32_t v;
            memcpy(&v, value, sizeof(v));
            if ((opt == SOCK_OPT_TCP_KEEPIDLE || opt == SOCK_OPT_TCP_KEEPINTVL) && (v == 0 || v > UINT32_MAX / 1000)) return SOCK_ERR_INVAL;
            if ((opt == SOCK_OPT_TCP_KEEPCNT || opt == SOCK_OPT_TCP_SYNCNT) && (v == 0 || v > 255)) return SOCK_ERR_INVAL;
            switch (opt) {
                case SOCK_OPT_TCP_KEEPIDLE:
                    s->options.tcp_keepidle = v;
                    s->options.flags |= opt;
                    break;
                case SOCK_OPT_TCP_KEEPINTVL:
                    s->options.tcp_keepintvl = v;
                    s->options.flags |= opt;
                    break;
                case SOCK_OPT_TCP_KEEPCNT:
                    s->options.tcp_keepcnt = v; 
                    s->options.flags |= opt;
                    break;
                case SOCK_OPT_TCP_SYNCNT:
                    s->options.tcp_syncnt = v;
                    s->options.flags |= opt;
                    break;
                case SOCK_OPT_TCP_USER_TIMEOUT:
                    s->options.tcp_user_timeout_ms = v;
                    s->options.flags |= opt;
                    break;
                case SOCK_OPT_TCP_QUICKACK:
                case SOCK_OPT_TCP_CORK:
                    if (v) s->options.flags |= opt;
                    else s->options.flags &= ~opt;
                    break;
                default: break;
            }
            if (s->flow.flow_generation) tcp_flow_apply_socket_options(&s->flow, &s->options, opt);
            return SOCK_OK;
        }
        case SOCK_OPT_TCP_NO_DELAY: {
            if (!value || len != sizeof(uint32_t)) return SOCK_ERR_INVAL;
            uint32_t v = 0;
            memcpy(&v, value, sizeof(v));
            if (v) s->options.flags |= SOCK_OPT_TCP_NO_DELAY;
            else s->options.flags &= ~SOCK_OPT_TCP_NO_DELAY;
            if (s->flow.flow_generation) tcp_flow_apply_socket_options(&s->flow, &s->options, opt);
            return SOCK_OK;
        }
        case SOCK_OPT_SEND_BUF_SIZE: {
            if (!value || len != sizeof(uint32_t)) return SOCK_ERR_INVAL;
            uint32_t v = 0;
            memcpy(&v, value, sizeof(v));
            if (!v) return SOCK_ERR_INVAL;
            s->options.send_buf_size = v;
            s->options.flags |= SOCK_OPT_SEND_BUF_SIZE;
            if (s->flow.flow_generation) tcp_flow_apply_socket_options(&s->flow, &s->options, opt);
            return SOCK_OK;
        }
        case SOCK_OPT_REUSEADDR: {
            if (s->localPort || s->connected || s->listening) return SOCK_ERR_STATE;
            return socket_common_options_set(&s->options, opt, value, len);
        }
        case SOCK_OPT_DONTROUTE: {
            if (s->flow.flow_generation) return SOCK_ERR_STATE;
            return socket_common_options_set(&s->options, opt, value, len);
        }
        case SOCK_OPT_NONBLOCK:
            return socket_common_options_set(&s->options, opt, value, len);
        case SOCK_OPT_TCP_SACK:
        case SOCK_OPT_TCP_DSACK:
            if (s->flow.flow_generation || s->listening) return SOCK_ERR_STATE;
            return socket_common_options_set(&s->options, opt, value, len);
        case SOCK_OPT_TCP_MAXSEG: {
            if (!value || len != sizeof(uint32_t)) return SOCK_ERR_INVAL;
            uint32_t v = 0;
            memcpy(&v, value, sizeof(v));
            if (v && (v < 256u || v > TCP_MAX_MSS)) return SOCK_ERR_INVAL;
            s->options.tcp_maxseg = v;
            if (v) s->options.flags |= SOCK_OPT_TCP_MAXSEG;
            else s->options.flags &= ~SOCK_OPT_TCP_MAXSEG;
            if (s->flow.flow_generation) tcp_flow_apply_socket_options(&s->flow, &s->options, opt);
            return SOCK_OK;
        }
        case SOCK_OPT_LINGER: {
            if (!value || len != sizeof(SocketLinger)) return SOCK_ERR_INVAL;
            SocketLinger linger;
            memcpy(&linger, value, sizeof(linger));
            if (linger.enabled) {
                linger.enabled = 1;
                s->options.linger = linger;
                s->options.flags |= SOCK_OPT_LINGER;
            } else {
                memset(&s->options.linger, 0, sizeof(s->options.linger));
                s->options.flags &= ~SOCK_OPT_LINGER;
            }
            return SOCK_OK;
        }
        case SOCK_OPT_REUSEPORT:
        case SOCK_OPT_MCAST_JOIN:
        case SOCK_OPT_MCAST_LEAVE:
        case SOCK_OPT_BROADCAST_ALLOWED:
        case SOCK_OPT_FILTER:
        case SOCK_OPT_SPECIAL:
        case SOCK_OPT_PACKET_TRUNK:
            return SOCK_ERR_UNSUP;
        case SOCK_OPT_BUF_SIZE: {
            if (!value || len != sizeof(uint32_t)) return SOCK_ERR_INVAL;
            if (s->flow.flow_generation || s->listening) return SOCK_ERR_STATE;
            uint32_t v = 0;
            memcpy(&v, value, sizeof(v));
            if (!v || v > TCP_RCV_BUF_MAX) return SOCK_ERR_INVAL;
            v = tcp_clamp_rcvbuf(v);
            return socket_common_options_set(&s->options, opt, &v, sizeof(v));
        }
        case SOCK_OPT_RECV_TIMEOUT:
        case SOCK_OPT_SEND_TIMEOUT:
        case SOCK_OPT_DEBUG:
            return socket_common_options_set(&s->options, opt, value, len);
        case SOCK_OPT_DONTFRAG:
        case SOCK_OPT_TTL: {
            int32_t rc = socket_common_options_set(&s->options, opt, value, len);
            if (rc != SOCK_OK) return rc;
            if (s->flow.flow_generation) tcp_flow_apply_socket_options(&s->flow, &s->options, opt);
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
            return socket_common_get_value(&s->remoteEP, sizeof(s->remoteEP), value, len);
        case SOCK_GET_BIND_SPEC:
            return socket_common_get_value(&s->bindSpec, sizeof(s->bindSpec), value, len);
        case SOCK_GET_LAST_RX_SPEC: {
            SockBindSpec spec;
            memset(&spec, 0, sizeof(spec));
            spec.kind = BIND_ANY;
            tcp_flow_t* flow = tcp_flow_from_ctx(&s->flow);
            if (flow) {
                if (flow->base.l3_id) {
                    spec.kind = BIND_L3;
                    spec.ver = flow->base.local.ver;
                    spec.l3_id = flow->base.l3_id;
                }
                tcp_flow_put(flow);
            }
            return socket_common_get_value(&spec, sizeof(spec), value, len);
        }
        case SOCK_GET_OPT_LINGER:
            return socket_common_get_value( &s->options.linger, sizeof(s->options.linger), value, len);
        default:
            break;
    }

    uint32_t v = 0;
    switch ((uint32_t)opt) {
        case SOCK_GET_BOUND:
            v = s->localPort != 0;
            break;
        case SOCK_GET_CONNECTED:
            v = tcp_socket_connection_state(s) > 0;
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
                for (uint32_t i = 0; i < TCP_MAX_TX_SEGS; ++i) if (flow->tx.txq[i].used) v += flow->tx.txq[i].retransmit_cnt;
                tcp_flow_put(flow);
            }
            break;
        }
        case SOCK_GET_TCP_CWND:
        case SOCK_GET_TCP_SSTHRESH:
        case SOCK_GET_TCP_RTO_MS:
        case SOCK_GET_IP_PATH_MTU: {
            tcp_flow_t* flow = tcp_flow_from_ctx(&s->flow);
            if (!flow) return SOCK_ERR_STATE;
            switch (opt) {
                case SOCK_GET_TCP_CWND:
                    v = flow->tx.cwnd;
                    break;
                case SOCK_GET_TCP_SSTHRESH:
                    v = flow->tx.ssthresh;
                    break;
                case SOCK_GET_TCP_RTO_MS:
                    v = flow->tx.rto;
                    break;
                case SOCK_GET_IP_PATH_MTU:
                    v = flow->tx.path_mss + (sizeof(tcp_hdr_t) + (flow->base.local.ver == IP_VER6 ? sizeof(ipv6_hdr_t) : sizeof(ipv4_hdr_t)));
                    break;
            }
            tcp_flow_put(flow);
            break;
        }
        case SOCK_GET_OPT_TCP_KEEPIDLE:
            if ((s->options.flags & SOCK_OPT_TCP_KEEPIDLE) && s->options.tcp_keepidle) v = s->options.tcp_keepidle;
            else v = SOCKET_DEFAULT_KEEPALIVE_MS / 1000;
            break;
        case SOCK_GET_OPT_TCP_KEEPINTVL:
            v = (s->options.flags & SOCK_OPT_TCP_KEEPINTVL) && s->options.tcp_keepintvl ? s->options.tcp_keepintvl : TCP_KEEPALIVE_INTERVAL_MS / 1000;
            break;
        case SOCK_GET_OPT_TCP_KEEPCNT:
            v = (s->options.flags & SOCK_OPT_TCP_KEEPCNT) && s->options.tcp_keepcnt ? s->options.tcp_keepcnt : TCP_KEEPALIVE_PROBES;
            break;
        case SOCK_GET_OPT_TCP_USER_TIMEOUT:
            v = (s->options.flags & SOCK_OPT_TCP_USER_TIMEOUT) ? s->options.tcp_user_timeout_ms : 0;
            break;
        case SOCK_GET_OPT_TCP_QUICKACK: { 
            tcp_flow_t* flow = tcp_flow_from_ctx(&s->flow);
            v = flow ? flow->timer.quickack : (s->options.flags & SOCK_OPT_TCP_QUICKACK) != 0;
            if (flow) tcp_flow_put(flow);
            break;
        }
        case SOCK_GET_OPT_TCP_CORK:
            v = (s->options.flags & SOCK_OPT_TCP_CORK) != 0;
            break;
        case SOCK_GET_OPT_TCP_SYNCNT:
            v = (s->options.flags & SOCK_OPT_TCP_SYNCNT) && s->options.tcp_syncnt ? s->options.tcp_syncnt : TCP_MAX_RETRANS;
            break;
        case SOCK_GET_TCP_URGENT_REMAINING: {
            tcp_flow_t* flow = tcp_flow_from_ctx(&s->flow);
            if (flow) {
                if (flow->rx.urg_valid && TCP_SEQ_GT(flow->rx.urg_seq, flow->rx.rcv_base)) v = flow->rx.urg_seq - flow->rx.rcv_base;
                tcp_flow_put(flow);
            }
            break;
        }
        case SOCK_GET_OPT_KEEPALIVE:
            v = (s->options.flags & SOCK_OPT_KEEPALIVE) != 0;
            break;
        case SOCK_GET_OPT_TCP_NO_DELAY:
            v = (s->options.flags & SOCK_OPT_TCP_NO_DELAY) != 0;
            break;
        case SOCK_GET_OPT_SEND_BUF_SIZE:
            v = s->options.send_buf_size;
            break;
        case SOCK_GET_OPT_TCP_MAXSEG:
            v = s->options.tcp_maxseg;
            break;
        case SOCK_GET_OPT_REUSEPORT:
        case SOCK_GET_MCAST_GROUPS:
        case SOCK_GET_OPT_BROADCAST_ALLOWED:
        case SOCK_GET_OPT_FILTER:
        case SOCK_GET_OPT_PACKET_TRUNK:
            return SOCK_ERR_UNSUP;
        case SOCK_GET_OPT_RECV_TIMEOUT:
        case SOCK_GET_OPT_SEND_TIMEOUT:
        case SOCK_GET_OPT_BUF_SIZE:
        case SOCK_GET_OPT_DEBUG:
        case SOCK_GET_OPT_DONTFRAG:
        case SOCK_GET_OPT_TTL:
        case SOCK_GET_OPT_NONBLOCK:
        case SOCK_GET_OPT_DONTROUTE:
        case SOCK_GET_OPT_REUSEADDR:
        case SOCK_GET_OPT_TCP_SACK:
        case SOCK_GET_OPT_TCP_DSACK:
            return socket_common_options_get(&s->options, opt, value, len);
        default:
            return SOCK_ERR_INVAL;
    }

    return socket_common_get_value(&v, sizeof(v), value, len);
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
    if (!socket_bind_prepare_spec(&spec, PROTO_TCP)) return SOCK_ERR_INVAL;

    int32_t bind_port = port;
    socket_bind_token_t token = 0;
    if (bind_port == 0) {
        bind_port = socket_bind_alloc_ephemeral(s->ownerSocket, PROTO_TCP, &spec, s->options.flags & SOCK_OPT_REUSEADDR, &token);
        if (bind_port < 0) return SOCK_ERR_NO_PORT;
    } else if (!socket_bind_insert(s->ownerSocket, PROTO_TCP, &spec, port, s->options.flags & SOCK_OPT_REUSEADDR, true, &token)) return SOCK_ERR_BOUND;

    s->bindSpec = spec;
    s->bindToken = token;
    l2_interface_t *link = spec.kind == BIND_L2 ? l2_interface_find_by_index(spec.ifindex) : NULL;
    s->bind_generation = link ? link->generation : 0;
    s->localPort = (uint16_t)bind_port;
    return SOCK_OK;
}

int32_t socket_listen_tcp(socket_impl_t sh, int32_t backlog) {
    tcp_socket_t* s = (tcp_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;
    if (!s->bindToken || s->connected) return SOCK_ERR_STATE;

    int32_t cap = backlog > TCP_MAX_BACKLOG ? TCP_MAX_BACKLOG : backlog;
    if (cap < 1) cap = 1;
    if (s->listening && s->pending.buffer && cring_capacity(&s->pending) == (uint32_t)cap) return SOCK_OK;

    ksocket_t** pending = (ksocket_t**)zalloc(sizeof(ksocket_t*)*cap);
    if (!pending) return SOCK_ERR_SYS;

    irq_flags_t irq = irq_save_disable();
    if (!socket_bind_tcp_listen(s->bindToken)) {
        irq_restore(irq);
        release(pending);
        return SOCK_ERR_BOUND;
    }
    CRingBuffer old_pending = s->pending;
    uint32_t old_len = cring_size(&old_pending);
    uint32_t keep = old_len < (uint32_t)cap ? old_len : (uint32_t)cap;
    cring_init(&s->pending, pending, (uint32_t)cap, sizeof(ksocket_t*));
    for (uint32_t i = 0; i < keep; i++) {
        ksocket_t** old = (ksocket_t**)cring_at(&old_pending, i);
        if (old) cring_push(&s->pending, old);
    }
    s->listening = true;
    irq_restore(irq);

    for (uint32_t i = keep; i < old_len; i++) {
        ksocket_t** owner = (ksocket_t**)cring_at(&old_pending, i);
        if (owner && *owner) tcp_socket_abort_pending(*owner);
    }
    if (old_pending.buffer) release(old_pending.buffer);
    return SOCK_OK;
}

//TODO replace polling with a socket wait queue when kernel events are present
ksocket_t* socket_accept_tcp(socket_impl_t sh) {
    tcp_socket_t* s = (tcp_socket_t*)sh;
    if (!s || !s->listening || !s->pending.buffer) return NULL;

    uint32_t start_ms = get_time();
    while (1) {
        irq_flags_t irq = irq_save_disable(); //TODO lock
        ksocket_t** head = s->listening ? (ksocket_t**)cring_front(&s->pending) : NULL;
        ksocket_t* owner = head ? *head : 0;
        if (owner) socket_core_ref(owner);
        irq_restore(irq);

        if (owner) {
            tcp_socket_t* client = (tcp_socket_t*)socket_core_impl(owner);
            int32_t state = client ? tcp_socket_connection_state(client) : -1;
            if (state != 0) {
                ksocket_t* ready = tcp_socket_pop_pending_at(s, 0, owner);
                socket_core_put(owner);
                if (ready) {
                    if (state > 0) return ready;
                    tcp_socket_abort_pending(ready);
                }
                continue;
            }
            socket_core_put(owner);
        }

        if (socket_core_is_closing(s->ownerSocket) || s->closed || !s->listening || !s->pending.buffer) return NULL;
        if (s->options.flags & SOCK_OPT_NONBLOCK) return NULL;
        if ((s->options.flags & SOCK_OPT_RECV_TIMEOUT) && s->options.recv_timeout_ms) {
            uint32_t elapsed = get_time() - start_ms;
            if (elapsed >= s->options.recv_timeout_ms) return NULL;
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

    if (s->listening) return SOCK_ERR_STATE;
    if (!dst || !dst->port) return SOCK_ERR_INVAL;
    if (dst->ver != IP_VER4 && dst->ver != IP_VER6) return SOCK_ERR_INVAL;
    if (!firewall_allows(PROTO_TCP, NET_CTRL_FIREWALL_OUT, dst, s->localPort, false)) return SOCK_ERR_PERM;
    if (s->flow.flow_generation) {
        if (s->remoteEP.port && !net_ep_equal(&s->remoteEP, dst)) return SOCK_ERR_STATE;
    } else {
        if (s->connected) return SOCK_ERR_STATE;

        net_l4_endpoint d = *dst;
        l3_id_t chosen_l3 = 0;

        if (d.ver == IP_VER6) {
            if (ipv6_is_unspecified(d.ip) || ipv6_is_multicast(d.ip)) return SOCK_ERR_INVAL;
            ipv6_tx_plan_t p6;
            if (!socket_bind_build_ipv6_tx_plan(&s->bindSpec, s->localPort != 0, d.ip, &p6)) return SOCK_ERR_NO_ROUTE;
            if ((s->options.flags & SOCK_OPT_DONTROUTE) && !ipv6_tx_plan_onlink(&p6, d.ip)) return SOCK_ERR_NO_ROUTE;
            chosen_l3 = p6.l3_id;
        } else if (d.ver == IP_VER4) {
            uint32_t dip = 0;
            memcpy(&dip, d.ip, 4);
            if (ipv4_is_unspecified(dip) || ipv4_is_multicast(dip) || ipv4_is_limited_broadcast(dip)) return SOCK_ERR_INVAL;
            ipv4_tx_plan_t p4;
            if (!socket_bind_build_ipv4_tx_plan(&s->bindSpec, s->localPort != 0, dip, &p4)) return SOCK_ERR_NO_ROUTE;
            if ((s->options.flags & SOCK_OPT_DONTROUTE) && !ipv4_tx_plan_onlink(&p4, dip)) return SOCK_ERR_NO_ROUTE;
            chosen_l3 = p4.l3_id;
        } else return SOCK_ERR_INVAL;

        if (!chosen_l3) return SOCK_ERR_NO_ROUTE;

        if (d.ver == IP_VER4) {
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(chosen_l3);
            if (!ipv4_l3_is_ready(v4)) return SOCK_ERR_SYS;
            uint32_t dip = 0;
            memcpy(&dip, d.ip, 4);
            if (ipv4_is_directed_broadcast(v4->ip, v4->mask, dip)) return SOCK_ERR_INVAL;
        } else if (d.ver == IP_VER6) {
            l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(chosen_l3);
            if (!ipv6_l3_is_tcp_usable(v6)) return SOCK_ERR_SYS;
        } else return SOCK_ERR_INVAL;

        bool ephemeral_allocated = false;
        if (s->localPort == 0) {
            if (!s->ownerSocket) return SOCK_ERR_SYS;
            socket_bind_token_t token = 0;
            int32_t p = socket_bind_alloc_ephemeral_l3(s->ownerSocket, PROTO_TCP, chosen_l3, s->options.flags & SOCK_OPT_REUSEADDR, &s->bindSpec, &token);
            if (p < 0) return SOCK_ERR_NO_PORT;
            s->localPort = (uint16_t)p;
            s->bindToken = token;
            ephemeral_allocated = true;
        }

        memset(&s->flow, 0, sizeof(s->flow));
        if (!tcp_handshake_l3(chosen_l3, s->localPort, &d, &s->flow, &s->options, socket_core_export_handle(s->ownerSocket))) {
            tcp_socket_reset_connection(s, true);

            if (ephemeral_allocated) {
                if (s->bindToken) {
                    socket_bind_remove(s->bindToken);
                    s->bindToken = 0;
                }
                s->localPort = 0;
                memset(&s->bindSpec, 0, sizeof(s->bindSpec));
                s->bindSpec.kind = BIND_ANY;
            }
            int32_t error = socket_core_take_error(s->ownerSocket);
            return error ? error : SOCK_ERR_SYS;
        }

        s->remoteEP = d;
    }

    uint32_t start_ms = get_time();
    uint32_t timeout_ms = (s->options.flags & SOCK_OPT_SEND_TIMEOUT) ? s->options.send_timeout_ms : 0;

    while (1) {
        int32_t state = tcp_socket_connection_state(s);
        if (state > 0) break;
        if (state < 0) {
            int32_t error = socket_core_take_error(s->ownerSocket);
            return error ? error : SOCK_ERR_STATE;
        }
        if (s->options.flags & SOCK_OPT_NONBLOCK) return SOCK_ERR_WOULDBLOCK;

        uint32_t now_ms = get_time();
        if (timeout_ms && now_ms - start_ms >= timeout_ms) {
            tcp_socket_reset_connection(s, true);
            return SOCK_ERR_TIMED_OUT;
        }
        msleep(5);
    }

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

    int32_t connection_state = tcp_socket_connection_state(s);
    if (connection_state <= 0 || !s->flow.flow_generation) {
        if (connection_state == 0) return SOCK_ERR_WOULDBLOCK;
        int32_t error = socket_core_take_error(s->ownerSocket);
        return error ? error : SOCK_ERR_STATE;
    }
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
            if (res != TCP_INVALID) return SOCK_ERR_SYS;
            int32_t error = socket_core_take_error(s->ownerSocket);
            return error ? error : SOCK_ERR_STATE;
        }
        if (s->flow.payload.size) return (int64_t)s->flow.payload.size;
        if (s->options.flags & SOCK_OPT_NONBLOCK) return SOCK_ERR_WOULDBLOCK;

        if ((s->options.flags & SOCK_OPT_SEND_TIMEOUT) && s->options.send_timeout_ms) {
            uint32_t now_ms = (uint32_t)get_time();
            uint32_t elapsed_ms = now_ms - start_ms;
            if (elapsed_ms >= s->options.send_timeout_ms) return SOCK_ERR_WOULDBLOCK;
            uint32_t wait_ms = s->options.send_timeout_ms - elapsed_ms;
            if (wait_ms > 5) wait_ms = 5;
            msleep(wait_ms);
        }else msleep(5);
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
    int32_t connection_state = tcp_socket_connection_state(s);
    if (connection_state < 0) {
        int32_t error = socket_core_take_error(s->ownerSocket);
        return error ? error : 0;
    }
    if (connection_state == 0 || !s->flow.flow_generation) return SOCK_ERR_WOULDBLOCK;

    //TODO add receive low water mark support when socket events exist
    uint32_t start_ms = (uint32_t)get_time();
    while (1) {
        uint32_t readable = tcp_flow_readable(&s->flow);
        bool closed = tcp_flow_recv_closed(&s->flow);
        if (readable || closed) {
            int64_t n = tcp_flow_read(&s->flow, buf, len);
            if (n > 0) return n;
            if (n == TCP_DISCONNECT) {
                int32_t error = socket_core_take_error(s->ownerSocket);
                return error ? error : 0;
            }
            if (n < 0) return SOCK_ERR_STATE;
        }
        if (closed || !s->connected) {
            int32_t error = socket_core_take_error(s->ownerSocket);
            return error ? error : 0;
        }
        if (s->options.flags & SOCK_OPT_NONBLOCK) return SOCK_ERR_WOULDBLOCK;
        if ((s->options.flags & SOCK_OPT_RECV_TIMEOUT) && s->options.recv_timeout_ms) {

            uint32_t now_ms = (uint32_t)get_time();
            uint32_t elapsed_ms = now_ms - start_ms;
            if (elapsed_ms >= s->options.recv_timeout_ms) return SOCK_ERR_WOULDBLOCK;

            uint32_t wait_ms = s->options.recv_timeout_ms - elapsed_ms;
            if (wait_ms > 5) wait_ms = 5;
            msleep(wait_ms);
        }else msleep(5);
    }
}

int32_t socket_close_tcp(socket_impl_t sh) {
    tcp_socket_t* s = (tcp_socket_t*)sh;
    if (!s) return SOCK_ERR_INVAL;
    if (s->closed) return SOCK_OK;

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_TCP;
    ev.action = NETLOG_ACT_CLOSE;
    ev.pid = socket_core_pid(s->ownerSocket);
    ev.local_port = s->localPort;
    ev.remote_ep = s->remoteEP;
    netlog_socket_event(&s->options, &ev);

    int32_t connection_state = tcp_socket_connection_state(s);
    bool linger = (s->options.flags & SOCK_OPT_LINGER) && s->options.linger.enabled;
    if (s->flow.flow_generation) {
        if (connection_state == 0 && !s->connected) tcp_flow_abort(&s->flow);
        else if (connection_state >= 0) {
            if (linger && s->options.linger.timeout_ms == 0) tcp_flow_abort(&s->flow);
            else {
                bool closed = tcp_flow_is_closed(&s->flow);
                if (connection_state > 0 && !closed) {
                    tcp_result_t flush_rc = tcp_flow_flush(&s->flow);
                    if (flush_rc == TCP_OK || flush_rc == TCP_WOULDBLOCK) {
                        tcp_result_t close_rc = tcp_flow_close(&s->flow);
                        if (close_rc != TCP_OK && close_rc != TCP_INVALID) tcp_flow_abort(&s->flow);
                    } else if (flush_rc != TCP_INVALID) tcp_flow_abort(&s->flow);
                    closed = tcp_flow_is_closed(&s->flow);
                }

                if (linger && !closed) {
                    if (s->options.flags & SOCK_OPT_NONBLOCK) return SOCK_ERR_WOULDBLOCK;
                    uint32_t start_ms = get_time();
                    while (!tcp_flow_is_closed(&s->flow)) {
                        if (get_time() - start_ms >= s->options.linger.timeout_ms) break;
                        msleep(5);
                    }
                    closed = tcp_flow_is_closed(&s->flow);
                }

                if (closed) {
                    tcp_result_t release_rc = tcp_flow_release_closed(&s->flow);
                    if (release_rc != TCP_OK && release_rc != TCP_INVALID && release_rc != TCP_BUSY) tcp_flow_abort(&s->flow);
                }
            }
        }
    }

    irq_flags_t irq = irq_save_disable();
    CRingBuffer pending = s->pending;
    memset(&s->pending, 0, sizeof(s->pending));
    s->listening = false;
    irq_restore(irq);
    if (pending.buffer) {
        for (uint32_t i = 0; i < cring_size(&pending); i++) {
            ksocket_t** owner = (ksocket_t**)cring_at(&pending, i);
            if (owner && *owner) tcp_socket_abort_pending(*owner);
        }
        release(pending.buffer);
    }

    if (s->bindToken) {
        socket_bind_remove(s->bindToken);
        s->bindToken = 0;
    }
    s->localPort = 0;
    memset(&s->bindSpec, 0, sizeof(s->bindSpec));
    s->bindSpec.kind = BIND_ANY;
    s->bind_generation = 0;
    tcp_socket_reset_connection(s, false);
    s->closed = true;
    return SOCK_OK;
}

void socket_tcp_l3_deleted(socket_impl_t sh, l3_id_t l3_id) {
    tcp_socket_t *s = (tcp_socket_t*)sh;
    if (!s || (s->bindSpec.kind != BIND_L3 && s->bindSpec.kind != BIND_IP) || s->bindSpec.l3_id != l3_id) return;
    tcp_socket_reset_connection(s, true);
    s->options.flags &= ~SOCK_OPT_LINGER;
    socket_close_tcp(sh);
}

void socket_tcp_l2_deleted(socket_impl_t sh, uint8_t ifindex, uint32_t generation) {
    tcp_socket_t *s = (tcp_socket_t*)sh;
    if (!s || s->bindSpec.kind != BIND_L2 || s->bindSpec.ifindex != ifindex || s->bind_generation != generation) return;
    tcp_socket_reset_connection(s, true);
    s->options.flags &= ~SOCK_OPT_LINGER;
    socket_close_tcp(sh);
}

void socket_destroy_tcp(socket_impl_t sh) {
    tcp_socket_t* s = (tcp_socket_t*)sh;
    if (!s) return;
    int32_t rc = socket_close_tcp(s);
    if (rc == SOCK_ERR_WOULDBLOCK) {
        s->options.flags &= ~(SOCK_OPT_NONBLOCK | SOCK_OPT_LINGER);
        memset(&s->options.linger, 0, sizeof(s->options.linger));
        rc = socket_close_tcp(s);
    }
    if (rc != SOCK_OK) tcp_socket_reset_connection(s, true);
    release(s);
}

const SocketOptions* socket_tcp_options(socket_impl_t sh) {
    tcp_socket_t* s = (tcp_socket_t*)sh;
    if (!s) return NULL;
    return &s->options;
}

int32_t tcp_accept_enqueue(ksocket_t* listener, ip_version_t ipver, l3_id_t l3_id, const void* src_ip_addr, const void* dst_ip_addr, uint16_t src_port, uint16_t dst_port) {
    if (!listener) return SOCK_ERR_STATE;
    tcp_socket_t* s = (tcp_socket_t*)socket_core_impl(listener);
    if (!s) return SOCK_ERR_STATE;
    if (!s->listening || !s->localPort || s->localPort != dst_port || !s->pending.buffer) return SOCK_ERR_STATE;

    for (uint32_t i = 0;;) {
        irq_flags_t irq = irq_save_disable();
        ksocket_t** item = (ksocket_t**)cring_at(&s->pending, i);
        ksocket_t* owner = item ? *item : NULL;
        if (owner) socket_core_ref(owner);
        irq_restore(irq);

        if (!owner) break;

        tcp_socket_t* client = (tcp_socket_t*)socket_core_impl(owner);
        if (client && client->flow.flow_generation) {
            bool readable = tcp_flow_readable(&client->flow) != 0;
            bool closed = tcp_flow_recv_closed(&client->flow);
            if (!closed || readable) {
                socket_core_put(owner);
                i++;
                continue;
            }
        }

        ksocket_t* removed = tcp_socket_pop_pending_at(s, i, owner);
        socket_core_put(owner);
        if (!removed) continue;
        tcp_socket_abort_pending(removed);
    }
    if ((uint32_t)tcp_socket_backlog_len(s) >= cring_capacity(&s->pending)) {
        tcp_stats.acceptq_drop_full++;
        return SOCK_ERR_WOULDBLOCK;
    }

    ksocket_t* child_owner = NULL;
    uint16_t owner_pid = socket_core_pid(s->ownerSocket);
    if (!socket_core_alloc(PROTO_TCP, SOCKET_SPECIAL_NONE, owner_pid, &child_owner)) return SOCK_ERR_WOULDBLOCK;

    tcp_socket_t* child = (tcp_socket_t*)socket_tcp_create(child_owner, NULL);
    if (!child) {
        socket_core_close_socket(child_owner);
        return SOCK_ERR_WOULDBLOCK;
    }
    child->options = s->options;
    child->options.flags &= ~SOCK_OPT_NONBLOCK;

    child->localPort = dst_port;
    make_ep(src_ip_addr, src_port, ipver, &child->remoteEP);
    child->bindSpec.kind = BIND_IP;
    child->bindSpec.ver = ipver;
    child->bindSpec.l3_id = l3_id;
    if (ipver == IP_VER4) memcpy(child->bindSpec.ip, dst_ip_addr, 4);
    else if (ipver == IP_VER6) ipv6_cpy(child->bindSpec.ip, dst_ip_addr);

    if (!tcp_get_ctx(dst_port, ipver, dst_ip_addr, child->remoteEP.ip, src_port, &child->flow)) {
        socket_destroy_tcp(child);
        socket_core_close_socket(child_owner);
        return SOCK_ERR_STATE;
    }

    if (!socket_core_attach_impl(child_owner, child, socket_destroy_tcp, socket_close_tcp, socket_setopt_tcp, socket_getopt_tcp)) {
        tcp_socket_reset_connection(child, false);
        socket_destroy_tcp(child);
        socket_core_close_socket(child_owner);
        return SOCK_ERR_STATE;
    }

    child->connected = true;
    tcp_flow_t* flow = tcp_flow_from_ctx(&child->flow);
    if (flow) {
        flow->base.owner_handle = socket_core_export_handle(child_owner);
        tcp_flow_put(flow);
    }
    socket_core_ref(child_owner);

    irq_flags_t irq = irq_save_disable();
    if (!s->listening || !s->pending.buffer || !cring_capacity(&s->pending)) {
        irq_restore(irq);
        tcp_socket_reset_connection(child, false);
        socket_core_close_socket(child_owner);
        socket_core_put(child_owner);
        return SOCK_ERR_STATE;
    }
    if (!cring_push(&s->pending, &child_owner)) {
        irq_restore(irq);
        tcp_socket_reset_connection(child, false);
        socket_core_close_socket(child_owner);
        socket_core_put(child_owner);
        tcp_stats.acceptq_drop_full++;
        return SOCK_ERR_WOULDBLOCK;
    }
    irq_restore(irq);
    return SOCK_OK;
}
