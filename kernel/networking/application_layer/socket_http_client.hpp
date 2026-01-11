#pragma once
#include "console/kio.h"
#include "networking/transport_layer/socket_tcp.hpp"
#include "http.h"
#include "std/std.h"
#include "net/socket_types.h"

class HTTPClient {
private:
    uint16_t pid;
    TCPSocket* sock;
    SocketExtraOptions log_opts;
    SocketExtraOptions* tcp_extra;

public:
    explicit HTTPClient(uint16_t pid_, const SocketExtraOptions* extra) : pid(pid_), sock(nullptr), log_opts{}, tcp_extra(nullptr) {
        if (extra) log_opts = *extra;

        const SocketExtraOptions* tcp_ptr = extra;
        if (extra && (log_opts.flags & SOCK_OPT_DEBUG)) {
            tcp_extra = (SocketExtraOptions*)malloc(sizeof(SocketExtraOptions));
            if (tcp_extra) {
                *tcp_extra = *extra;
                tcp_extra->flags &= ~SOCK_OPT_DEBUG;
                tcp_ptr = tcp_extra;
            }
        }

        sock = (TCPSocket*)malloc(sizeof(TCPSocket));
        if (sock) new (sock) TCPSocket(SOCK_ROLE_CLIENT, pid, tcp_ptr);
    }

    ~HTTPClient() {close();}

    int32_t connect(SockDstKind kind, const void* dst, uint16_t port) {
        uint16_t p = port;
        int32_t r = sock ? sock->connect(kind, dst, p) : SOCK_ERR_STATE;

        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_HTTP_CLIENT;
        ev.action = NETLOG_ACT_CONNECT;
        ev.pid = pid;
        ev.dst_kind = kind;
        ev.u0 = p;
        if (kind == DST_DOMAIN) ev.s0 = (const char*)dst;
        if (kind == DST_ENDPOINT && dst) ev.dst_ep = *(const net_l4_endpoint*)dst;
        ev.i0 = r;

        if (sock) {
            ev.local_port = sock->get_local_port();
            ev.remote_ep = sock->get_remote_ep();
            if (ev.remote_ep.ver) ev.dst_ep = ev.remote_ep;
        }

        netlog_socket_event(&log_opts, &ev);
        return r;
    }

    HTTPResponseMsg send_request(const HTTPRequestMsg& req) {
        HTTPResponseMsg resp{};
        if (!sock) {
            resp.status_code = (HttpError)SOCK_ERR_STATE;
            return resp;
        }
        
        string out = http_request_builder(&req);
        uint32_t out_len = out.length;

        uint32_t off = 0;
        int64_t sent = 0;
        while (off < out_len) {
            int64_t r = sock->send(out.data + off, out_len - off);
            if (r == TCP_WOULDBLOCK) {
                msleep(5);
                continue;
            }
            if (r < 0) {
                sent = r;
                break;
            }
            off += (uint32_t)r;
        }
        if (sent >= 0) sent = (int64_t)off;

        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_HTTP_CLIENT;
        ev.action = NETLOG_ACT_HTTP_SEND_REQUEST;
        ev.pid = pid;
        ev.u0 = out_len;
        ev.i0 = sent;
        ev.local_port = sock->get_local_port();
        ev.remote_ep = sock->get_remote_ep();

        char pathbuf[128];
        if (req.path.length && req.path.data) {
            uint32_t n = req.path.length;
            if (n > sizeof(pathbuf) - 1) n = sizeof(pathbuf) - 1;
            memcpy(pathbuf, req.path.data, n);
            pathbuf[n] = 0;
            ev.s0 = pathbuf;
        }

        netlog_socket_event(&log_opts, &ev);
        free_sized(out.data, out.mem_length);

        if (sent < 0) {
            resp.status_code = (HttpError)sent;
            return resp;
        }

        string buf = string_repeat('\0', 0);
        char tmp[512];
        int hdr_end = -1;

        while (hdr_end < 0) {
            int64_t r = sock->recv(tmp, sizeof(tmp));
            if (r == TCP_WOULDBLOCK) {
                msleep(10);
                continue;
            }
            if (r < 0) {
                free_sized(buf.data, buf.mem_length);
                resp.status_code = (HttpError)r;
                return resp;
            }
            if (r == 0) {
                free_sized(buf.data, buf.mem_length);
                resp.status_code = (HttpError)SOCK_ERR_PROTO;
                return resp;
            }
            string_append_bytes(&buf, tmp, (uint32_t)r);
            hdr_end = find_crlfcrlf(buf.data, buf.length);
        }

        uint32_t i = 0;
        while (i < (uint32_t)hdr_end && buf.data[i] != ' ') i++;
        uint32_t code = 0, j = i+1;
        while (j < (uint32_t)hdr_end && buf.data[j] >= '0' && buf.data[j] <= '9') {
            code = code*10 + (buf.data[j]-'0');
            ++j;
        }
        resp.status_code = (HttpError)code;
        while (j < (uint32_t)hdr_end && buf.data[j]==' ') ++j;
        if (j < (uint32_t)hdr_end) {
            uint32_t rlen = hdr_end - j;
            resp.reason = string_repeat('\0', 0);
            string_append_bytes(&resp.reason, buf.data+j, rlen);
        }

        HTTPHeader *extras = nullptr;
        uint32_t extra_count = 0;
        int status_line_end = strindex((char*)buf.data, "\r\n");
        http_header_parser(
            (char*)buf.data + status_line_end + 2,
            buf.length - (uint32_t)(status_line_end + 2),
            &resp.headers_common,
            &extras,
            &extra_count);
        resp.extra_headers = extras;
        resp.extra_header_count = extra_count;

        uint32_t body_start = hdr_end + 4;
        uint32_t have = (buf.length > body_start) ? buf.length - body_start : 0;

        uint32_t need = resp.headers_common.length;
        if (need > 0) {
            while (have < need) {
                int64_t r = sock->recv(tmp, sizeof(tmp));
                if (r == TCP_WOULDBLOCK) { msleep(10); continue; }
                if (r < 0) break;
                if (r == 0) break;
                string_append_bytes(&buf, tmp, (uint32_t)r);
                have += (uint32_t)r;
            }
        }
        if (have > 0) {
            char *body_copy = (char*)malloc(have);
            if (body_copy) {
                memcpy(body_copy, buf.data + body_start, have);
                resp.body.ptr = (uintptr_t)body_copy;
                resp.body.size = have;
            }
        }

        netlog_socket_event_t ev1{};
        ev1.comp = NETLOG_COMP_HTTP_CLIENT;
        ev1.action = NETLOG_ACT_HTTP_RECV_RESPONSE;
        ev1.pid = pid;
        ev1.u0 = (uint32_t)resp.status_code;
        ev1.u1 = (uint32_t)resp.body.size;
        ev1.local_port = sock->get_local_port();
        ev1.remote_ep = sock->get_remote_ep();
        netlog_socket_event(&log_opts, &ev1);

        free_sized(buf.data, buf.mem_length);
        return resp;
    }

    int32_t close() {
        int32_t r = SOCK_ERR_STATE;
        if (sock) r = sock->close();

        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_HTTP_CLIENT;
        ev.action = NETLOG_ACT_CLOSE;
        ev.pid = pid;
        ev.i0 = r;

        if (sock) {
            ev.local_port = sock->get_local_port();
            ev.remote_ep = sock->get_remote_ep();
        }

        netlog_socket_event(&log_opts, &ev);

        if (sock) sock->~TCPSocket();
        if (sock) free_sized(sock, sizeof(TCPSocket));
        sock = nullptr;

        if (tcp_extra) free_sized(tcp_extra, sizeof(SocketExtraOptions));
        tcp_extra = nullptr;

        log_opts.flags &= ~SOCK_OPT_DEBUG;
        return r;
    }
};
