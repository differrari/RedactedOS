#include "net_logger.h"
#include "console/kio.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/application_layer/http.h"

static const char* http_method_str(uint32_t m) {
    switch ((HTTPMethod)m) {
        case HTTP_METHOD_GET: return "GET";
        case HTTP_METHOD_POST: return "POST";
        case HTTP_METHOD_PUT: return "PUT";
        case HTTP_METHOD_DELETE: return "DELETE";
        default: return "";
    }
}

static const char* comp_str(netlog_component_t c) {
    switch (c) {
        case NETLOG_COMP_UDP: return "UDP";
        case NETLOG_COMP_TCP: return "TCP";
        case NETLOG_COMP_HTTP_CLIENT: return "HTTP-C";
        case NETLOG_COMP_HTTP_SERVER: return "HTTP-S";
        default: return "";
    }
}

static const char* act_str(netlog_action_t a) {
    switch (a) {
        case NETLOG_ACT_BIND: return "bind";
        case NETLOG_ACT_CONNECT: return "connect";
        case NETLOG_ACT_CONNECTED: return "connected";
        case NETLOG_ACT_LISTEN: return "listen";
        case NETLOG_ACT_ACCEPT: return "accept";
        case NETLOG_ACT_SEND: return "send";
        case NETLOG_ACT_SENDTO: return "sendto";
        case NETLOG_ACT_RECV: return "recv";
        case NETLOG_ACT_RECVFROM: return "recvfrom";
        case NETLOG_ACT_CLOSE: return "close";
        case NETLOG_ACT_HTTP_SEND_REQUEST: return "send_request";
        case NETLOG_ACT_HTTP_RECV_RESPONSE: return "recv_response";
        case NETLOG_ACT_HTTP_RECV_REQUEST: return "recv_request";
        case NETLOG_ACT_HTTP_SEND_RESPONSE: return "send_response";
        default: return "event";
    }
}

static const char* bind_kind_str(SockBindKind k) {
    switch (k) {
        case BIND_L3: return "L3";
        case BIND_L2: return "L2";
        case BIND_IP: return "IP";
        case BIND_ANY: return "ANY";
        default: return "";
    }
}

static const char* dst_kind_str(SockDstKind k) {
    switch (k) {
        case DST_ENDPOINT: return "EP";
        case DST_DOMAIN: return "DNS";
        default: return "";
    }
}

void netlog_socket_event(const SocketExtraOptions* extra, const netlog_socket_event_t* e) {
    if (!extra) return;
    if (!e) return;

    if ((extra->flags & SOCK_OPT_DEBUG) == 0) return;

    SockDebugLevel lvl = extra->debug_level;
    if (lvl > SOCK_DBG_ALL) lvl = SOCK_DBG_ALL;

    const char* c = comp_str(e->comp);
    const char* a = act_str(e->action);

    if (lvl == SOCK_DBG_LOW) {
        if (e->action == NETLOG_ACT_CONNECTED) {
            kprintf("[NET][%s] %s lp=%u rp=%u", c, a, (uint32_t)e->u0, (uint32_t)e->u1);
            return;
        }
        if (e->action == NETLOG_ACT_SEND || e->action == NETLOG_ACT_RECV || e->action == NETLOG_ACT_SENDTO || e->action == NETLOG_ACT_RECVFROM) {
            kprintf("[NET][%s] %s n=%u", c, a, (uint32_t)e->u0);
            return;
        }
        if (e->action == NETLOG_ACT_HTTP_SEND_REQUEST) {
            kprintf("[NET][%s] %s bytes=%u sent=%lld", c, a, (uint32_t)e->u0, (long long)e->i0);
            return;
        }
        if (e->action == NETLOG_ACT_HTTP_RECV_RESPONSE) {
            kprintf("[NET][%s] %s code=%u body=%u", c, a, (uint32_t)e->u0, (uint32_t)e->u1);
            return;
        }

        if (e->action == NETLOG_ACT_CONNECT && (e->comp == NETLOG_COMP_HTTP_CLIENT || e->comp == NETLOG_COMP_HTTP_SERVER)) {
            kprintf("[NET][%s] %s port=%u r=%lld", c, a, (uint32_t)e->u0, (long long)e->i0);
            return;
        }

        if (e->action == NETLOG_ACT_HTTP_RECV_REQUEST) {
            kprintf("[NET][%s] %s method=%s path_len=%u body=%u", c, a, http_method_str(e->u0), (uint32_t)e->u1, (uint32_t)e->i0);
            return;
        }

        if (e->action == NETLOG_ACT_HTTP_SEND_RESPONSE) {
            kprintf("[NET][%s] %s code=%u bytes=%u sent=%lld", c, a, (uint32_t)e->u0, (uint32_t)e->u1, (long long)e->i0);
            return;
        }

        kprintf("[NET][%s] %s", c, a);
        return;
    }

    if (lvl == SOCK_DBG_MEDIUM) {
        if (e->action == NETLOG_ACT_BIND) {
            kprintf("[NET][%s] %s port=%u kind=%s", c, a, (uint32_t)e->u0, bind_kind_str(e->bind_spec.kind));
            return;
        }

        if (e->action == NETLOG_ACT_CONNECT) {
            if (e->comp == NETLOG_COMP_HTTP_CLIENT || e->comp == NETLOG_COMP_HTTP_SERVER) {
                char dip[80];
                bool dv6 = false;
                uint16_t dport = 0;
                net_ep_split(&e->dst_ep, dip, (int)sizeof(dip), &dv6, &dport);

                if (e->dst_kind == DST_DOMAIN && e->s0) {
                    if (dv6) kprintf("[NET][%s] %s host=%s port=%u dst=[%s]:%u r=%lld", c, a, e->s0, (uint32_t)e->u0, dip, (uint32_t)dport, (long long)e->i0);
                    else kprintf("[NET][%s] %s host=%s port=%u dst=%s:%u r=%lld", c, a, e->s0, (uint32_t)e->u0, dip, (uint32_t)dport, (long long)e->i0);
                } else {
                    if (dv6) kprintf("[NET][%s] %s dst=[%s]:%u r=%lld", c, a, dip, (uint32_t)dport, (long long)e->i0);
                    else kprintf("[NET][%s] %s dst=%s:%u r=%lld", c, a, dip, (uint32_t)dport, (long long)e->i0);
                }
            } else {
                kprintf("[NET][%s] %s kind=%s port=%u", c, a, dst_kind_str(e->dst_kind), (uint32_t)e->u0);
            }
            return;
        }

        if (e->action == NETLOG_ACT_HTTP_RECV_REQUEST) {
            char rip[80];
            bool rv6 = false;
            uint16_t rport = 0;
            net_ep_split(&e->remote_ep, rip, (int)sizeof(rip), &rv6, &rport);

            if (rv6) kprintf("[NET][%s] %s method=%s path_len=%u body=%u remote=[%s]:%u", c, a, http_method_str(e->u0), (uint32_t)e->u1, (uint32_t)e->i0, rip, (uint32_t)rport);
            else kprintf("[NET][%s] %s method=%s path_len=%u body=%u remote=%s:%u", c, a, http_method_str(e->u0), (uint32_t)e->u1, (uint32_t)e->i0, rip, (uint32_t)rport);
            return;
        }

        if (e->action == NETLOG_ACT_HTTP_SEND_RESPONSE) {
            char rip[80];
            bool rv6 = false;
            uint16_t rport = 0;
            net_ep_split(&e->remote_ep, rip, (int)sizeof(rip), &rv6, &rport);

            if (rv6) kprintf("[NET][%s] %s code=%u bytes=%u sent=%lld remote=[%s]:%u", c, a, (uint32_t)e->u0, (uint32_t)e->u1, (long long)e->i0, rip, (uint32_t)rport);
            else kprintf("[NET][%s] %s code=%u bytes=%u sent=%lld remote=%s:%u", c, a, (uint32_t)e->u0, (uint32_t)e->u1, (long long)e->i0, rip, (uint32_t)rport);
            return;
        }

        if (e->action == NETLOG_ACT_LISTEN) {
            kprintf("[NET][%s] %s backlog=%u", c, a, (uint32_t)e->u0);
            return;
        }

        if (e->action == NETLOG_ACT_ACCEPT) {
            char rip[80];
            bool rv6 = false;
            uint16_t rport = 0;
            net_ep_split(&e->remote_ep, rip, (int)sizeof(rip), &rv6, &rport);

            if (rv6) kprintf("[NET][%s] %s client=%p remote=[%s]:%u", c, a, (void*)(uintptr_t)e->i0, rip, (uint32_t)rport);
            else kprintf("[NET][%s] %s client=%p remote=%s:%u", c, a, (void*)(uintptr_t)e->i0, rip, (uint32_t)rport);
            return;
        }

        if (e->action == NETLOG_ACT_CONNECTED) {
            kprintf("[NET][%s] %s local=%u remote=%u", c, a, (uint32_t)e->u0, (uint32_t)e->u1);
            return;
        }

        if (e->action == NETLOG_ACT_SEND || e->action == NETLOG_ACT_RECV) {
            kprintf("[NET][%s] %s n=%u", c, a, (uint32_t)e->u0);
            return;
        }

        if (e->action == NETLOG_ACT_SENDTO) {
            kprintf("[NET][%s] %s kind=%s port=%u n=%u", c, a, dst_kind_str(e->dst_kind), (uint32_t)e->u0, (uint32_t)e->u1);
            return;
        }

        if (e->action == NETLOG_ACT_RECVFROM) {
            kprintf("[NET][%s] %s cap=%u", c, a, (uint32_t)e->u0);
            return;
        }

        if (e->action == NETLOG_ACT_HTTP_SEND_REQUEST) {
            char rip[80];
            bool rv6 = false;
            uint16_t rport = 0;
            net_ep_split(&e->remote_ep, rip, (int)sizeof(rip), &rv6, &rport);

            if (rv6) kprintf("[NET][%s] %s bytes=%u sent=%lld remote=[%s]:%u", c, a, (uint32_t)e->u0, (long long)e->i0, rip, (uint32_t)rport);
            else kprintf("[NET][%s] %s bytes=%u sent=%lld remote=%s:%u", c, a, (uint32_t)e->u0, (long long)e->i0, rip, (uint32_t)rport);
            return;
        }

        if (e->action == NETLOG_ACT_HTTP_RECV_RESPONSE) {
            char rip[80];
            bool rv6 = false;
            uint16_t rport = 0;
            net_ep_split(&e->remote_ep, rip, (int)sizeof(rip), &rv6, &rport);

            if (rv6) kprintf("[NET][%s] %s code=%u body=%u remote=[%s]:%u", c, a, (uint32_t)e->u0, (uint32_t)e->u1, rip, (uint32_t)rport);
            else kprintf("[NET][%s] %s code=%u body=%u remote=%s:%u", c, a, (uint32_t)e->u0, (uint32_t)e->u1, rip, (uint32_t)rport);
            return;
        }

        kprintf("[NET][%s] %s", c, a);
        return;
    }

    char dst_ip[80];
    char rem_ip[80];
    bool dst_v6 = false;
    bool rem_v6 = false;
    uint16_t dst_port = 0;
    uint16_t rem_port = 0;
    net_ep_split(&e->dst_ep, dst_ip, (int)sizeof(dst_ip), &dst_v6, &dst_port);
    net_ep_split(&e->remote_ep, rem_ip, (int)sizeof(rem_ip), &rem_v6, &rem_port);

    if (e->action == NETLOG_ACT_BIND) {
        kprintf("[NET][%s] %s port=%u kind=%s l3=%u if=%u", c, a, (uint32_t)e->u0, bind_kind_str(e->bind_spec.kind), (uint32_t)e->bind_spec.l3_id, (uint32_t)e->bind_spec.ifindex);
        return;
    }

    if (e->action == NETLOG_ACT_CONNECT) {
        if (e->dst_kind == DST_DOMAIN && e->s0) {
            if (dst_v6) kprintf("[NET][%s] %s host=%s port=%u dst=[%s]:%u r=%lld", c, a, e->s0, (uint32_t)e->u0, dst_ip, (uint32_t)dst_port, (long long)e->i0);
            else kprintf("[NET][%s] %s host=%s port=%u dst=%s:%u r=%lld", c, a, e->s0, (uint32_t)e->u0, dst_ip, (uint32_t)dst_port, (long long)e->i0);
        } else if (dst_v6) {
            kprintf("[NET][%s] %s dst=[%s]:%u r=%lld", c, a, dst_ip, (uint32_t)dst_port, (long long)e->i0);
        } else {
            kprintf("[NET][%s] %s dst=%s:%u r=%lld", c, a, dst_ip, (uint32_t)dst_port, (long long)e->i0);
        }
        return;
    }

    if (e->action == NETLOG_ACT_SENDTO) {
        if (e->dst_kind == DST_DOMAIN && e->s0)
            kprintf("[NET][%s] %s host=%s port=%u n=%u", c, a, e->s0, (uint32_t)e->u0, (uint32_t)e->u1);
        else if (dst_v6)
            kprintf("[NET][%s] %s dst=[%s]:%u n=%u", c, a, dst_ip, (uint32_t)dst_port, (uint32_t)e->u1);
        else
            kprintf("[NET][%s] %s dst=%s:%u n=%u", c, a, dst_ip, (uint32_t)dst_port, (uint32_t)e->u1);
        return;
    }

    if (e->action == NETLOG_ACT_CONNECTED) {
        if (rem_v6)
            kprintf("[NET][%s] %s local=%u remote=[%s]:%u", c, a, (uint32_t)e->u0, rem_ip, (uint32_t)rem_port);
        else
            kprintf("[NET][%s] %s local=%u remote=%s:%u", c, a, (uint32_t)e->u0, rem_ip, (uint32_t)rem_port);
        return;
    }

    if (e->action == NETLOG_ACT_SEND) {
        if (rem_v6)
            kprintf("[NET][%s] %s n=%u remote=[%s]:%u", c, a, (uint32_t)e->u0, rem_ip, (uint32_t)rem_port);
        else
            kprintf("[NET][%s] %s n=%u remote=%s:%u", c, a, (uint32_t)e->u0, rem_ip, (uint32_t)rem_port);
        return;
    }

    if (e->action == NETLOG_ACT_RECV) {
        if (rem_v6)
            kprintf("[NET][%s] %s cap=%u remote=[%s]:%u", c, a, (uint32_t)e->u0, rem_ip, (uint32_t)rem_port);
        else
            kprintf("[NET][%s] %s cap=%u remote=%s:%u", c, a, (uint32_t)e->u0, rem_ip, (uint32_t)rem_port);
        return;
    }

    if (e->action == NETLOG_ACT_RECVFROM) {
        kprintf("[NET][%s] %s cap=%u", c, a, (uint32_t)e->u0);
        return;
    }

    if (e->action == NETLOG_ACT_CLOSE) {
        if (rem_v6)
            kprintf("[NET][%s] %s lp=%u remote=[%s]:%u", c, a, (uint32_t)e->local_port, rem_ip, (uint32_t)rem_port);
        else
            kprintf("[NET][%s] %s lp=%u remote=%s:%u", c, a, (uint32_t)e->local_port, rem_ip, (uint32_t)rem_port);
        return;
    }

    if (e->action == NETLOG_ACT_ACCEPT) {
        if (rem_v6)
            kprintf("[NET][%s] %s client=%p remote=[%s]:%u", c, a, (void*)(uintptr_t)e->i0, rem_ip, (uint32_t)rem_port);
        else
            kprintf("[NET][%s] %s client=%p remote=%s:%u", c, a, (void*)(uintptr_t)e->i0, rem_ip, (uint32_t)rem_port);
        return;
    }

    if (e->action == NETLOG_ACT_LISTEN) {
        kprintf("[NET][%s] %s backlog=%u", c, a, (uint32_t)e->u0);
        return;
    }

    if (e->action == NETLOG_ACT_HTTP_SEND_REQUEST) {
        if (rem_v6) {
            if (e->s0) kprintf("[NET][%s] %s path=%s bytes=%u sent=%lld remote=[%s]:%u", c, a, e->s0, (uint32_t)e->u0, (long long)e->i0, rem_ip, (uint32_t)rem_port);
            else kprintf("[NET][%s] %s bytes=%u sent=%lld remote=[%s]:%u", c, a, (uint32_t)e->u0, (long long)e->i0, rem_ip, (uint32_t)rem_port);
        } else {
            if (e->s0) kprintf("[NET][%s] %s path=%s bytes=%u sent=%lld remote=%s:%u", c, a, e->s0, (uint32_t)e->u0, (long long)e->i0, rem_ip, (uint32_t)rem_port);
            else kprintf("[NET][%s] %s bytes=%u sent=%lld remote=%s:%u", c, a, (uint32_t)e->u0, (long long)e->i0, rem_ip, (uint32_t)rem_port);
        }
        return;
    }

    if (e->action == NETLOG_ACT_HTTP_RECV_RESPONSE) {
        if (rem_v6)
            kprintf("[NET][%s] %s code=%u body=%u remote=[%s]:%u", c, a, (uint32_t)e->u0, (uint32_t)e->u1, rem_ip, (uint32_t)rem_port);
        else
            kprintf("[NET][%s] %s code=%u body=%u remote=%s:%u", c, a, (uint32_t)e->u0, (uint32_t)e->u1, rem_ip, (uint32_t)rem_port);
        return;
    }

    if (e->action == NETLOG_ACT_HTTP_RECV_REQUEST) {
        if (rem_v6) {
            if (e->s0) kprintf("[NET][%s] %s method=%s path=%s body=%u remote=[%s]:%u", c, a, http_method_str(e->u0), e->s0, (uint32_t)e->i0, rem_ip, (uint32_t)rem_port);
            else kprintf("[NET][%s] %s method=%u path_len=%u body=%u remote=[%s]:%u", c, a, (uint32_t)e->u0, (uint32_t)e->u1, (uint32_t)e->i0, rem_ip, (uint32_t)rem_port);
        } else {
            if (e->s0) kprintf("[NET][%s] %s method=%s path=%s body=%u remote=%s:%u", c, a, http_method_str(e->u0), e->s0, (uint32_t)e->i0, rem_ip, (uint32_t)rem_port);
            else kprintf("[NET][%s] %s method=%u path_len=%u body=%u remote=%s:%u", c, a, (uint32_t)e->u0, (uint32_t)e->u1, (uint32_t)e->i0, rem_ip, (uint32_t)rem_port);
        }
        return;
    }

    if (e->action == NETLOG_ACT_HTTP_SEND_RESPONSE) {
        if (rem_v6)
            kprintf("[NET][%s] %s code=%u bytes=%u sent=%lld remote=[%s]:%u", c, a, (uint32_t)e->u0, (uint32_t)e->u1, (long long)e->i0, rem_ip, (uint32_t)rem_port);
        else
            kprintf("[NET][%s] %s code=%u bytes=%u sent=%lld remote=%s:%u", c, a, (uint32_t)e->u0, (uint32_t)e->u1, (long long)e->i0, rem_ip, (uint32_t)rem_port);
        return;
    }

    kprintf("[NET][%s] %s", c, a);
}