#pragma once
#include "console/kio.h"
#include "networking/transport_layer/socket_tcp.hpp"
#include "http.h"
#include "std/std.h"
#include "net/socket_types.h"


class HTTPServer {
private:
    uint16_t pid;
    TCPSocket* sock;
    SocketExtraOptions log_opts;
    SocketExtraOptions* tcp_extra;

public:
    explicit HTTPServer(uint16_t pid_, const SocketExtraOptions* extra) : pid(pid_), sock(nullptr), log_opts{}, tcp_extra(nullptr) {
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
        if (sock) new (sock) TCPSocket(SOCK_ROLE_SERVER, pid, tcp_ptr);
    }

    ~HTTPServer() { close(); }

    int32_t bind(const SockBindSpec& spec, uint16_t port) {
        uint16_t p = port;
        int32_t r = sock ? sock->bind(spec, p) : SOCK_ERR_STATE;

        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_HTTP_SERVER;
        ev.action = NETLOG_ACT_BIND;
        ev.pid = pid;
        ev.u0 = p;
        ev.i0 = r;
        netlog_socket_event(&log_opts, &ev);
        return r;
    }

    int32_t listen(int backlog = 4) {
        int b = backlog;
        int32_t r = sock ? sock->listen(b) : SOCK_ERR_STATE;

        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_HTTP_SERVER;
        ev.action = NETLOG_ACT_LISTEN;
        ev.pid = pid;
        ev.u0 = (uint32_t)b;
        ev.i0 = r;
        netlog_socket_event(&log_opts, &ev);
        return r;
    }

    TCPSocket* accept() {
        TCPSocket* c = sock ? sock->accept() : nullptr;
        if (c) {
            netlog_socket_event_t ev{};
            ev.comp = NETLOG_COMP_HTTP_SERVER;
            ev.action = NETLOG_ACT_ACCEPT;
            ev.pid = pid;
            ev.i0 = (int64_t)(uintptr_t)c;
            ev.local_port = c->get_local_port();
            ev.remote_ep = c->get_remote_ep();
            netlog_socket_event(&log_opts, &ev);
        }
        return c;
    }

    HTTPRequestMsg recv_request(TCPSocket* client) {
        HTTPRequestMsg req{};
        if (!client) return req;

        string buf = string_repeat('\0', 0);
        char tmp[512];
        int hdr_end = -1;

        while (hdr_end < 0) {
            int64_t r = client->recv(tmp, sizeof(tmp));
            if (r == TCP_WOULDBLOCK) {
                msleep(10);
                continue;
            }
            if (r <= 0) {
                free_sized(buf.data, buf.mem_length);
                return req;
            }
            string_append_bytes(&buf, tmp, (uint32_t)r);
            hdr_end = find_crlfcrlf(buf.data, buf.length);
        }

        uint32_t line_end = 0;
        while (line_end + 1u < (uint32_t)hdr_end) {
            if (buf.data[line_end] == '\r' && buf.data[line_end + 1u] == '\n')
                break;
            ++line_end;
        }

        uint32_t p = 0;
        while (p + 1u < line_end && buf.data[p] == '\r' && buf.data[p + 1u] == '\n')
            p += 2;

        uint32_t i = p;
        while (i < line_end && buf.data[i] != ' ') ++i;

        const char* method_tok = buf.data + p;
        uint32_t mlen = i > p ? (i - p) : 0;

        if (mlen == 3 && memcmp(method_tok, "GET", 3) == 0) req.method = HTTP_METHOD_GET;
        else if (mlen == 4 && memcmp(method_tok, "POST", 4) == 0) req.method = HTTP_METHOD_POST;
        else if (mlen == 3 && memcmp(method_tok, "PUT", 3) == 0) req.method = HTTP_METHOD_PUT;
        else if (mlen == 6 && memcmp(method_tok, "DELETE", 6) == 0) req.method = HTTP_METHOD_DELETE;
        else req.method = HTTP_METHOD_GET;

        uint32_t j = (i < line_end) ? (i + 1u) : line_end;
        uint32_t path_start = j;
        while (j < line_end && buf.data[j] != ' ') ++j;
        req.path = string_repeat('\0', 0);
        string_append_bytes(&req.path, buf.data + path_start, j - path_start);

        if (req.path.length >= 7 && memcmp(req.path.data, "http://", 7) == 0) {
            uint32_t k = 7;
            while (k < req.path.length && req.path.data[k] != '/') ++k;
            if (k < req.path.length) {
                string newp = string_repeat('\0', 0);
                string_append_bytes(&newp, req.path.data + k, req.path.length - k);
                free_sized(req.path.data, req.path.mem_length);
                req.path = newp;
            }
        } else if (req.path.length >= 8 && memcmp(req.path.data, "https://", 8) == 0) {
            uint32_t k = 8;
            while (k < req.path.length && req.path.data[k] != '/') ++k;
            if (k < req.path.length) {
                string newp = string_repeat('\0', 0);
                string_append_bytes(&newp, req.path.data + k, req.path.length - k);
                free_sized(req.path.data, req.path.mem_length);
                req.path = newp;
            }
        }

        int status_line_end = (int)line_end;
        http_header_parser(
            (char*)buf.data + status_line_end + 2,
            (uint32_t)hdr_end - (uint32_t)(status_line_end + 2),
            &req.headers_common,
            &req.extra_headers,
            &req.extra_header_count
        );

        uint32_t body_start = hdr_end + 4;
        uint32_t have = buf.length > body_start ? buf.length - body_start : 0;
        uint32_t need = req.headers_common.length;

        if (need > 0) {
            while (have < need) {
                int64_t r = client->recv(tmp, sizeof(tmp));
                if (r == TCP_WOULDBLOCK) {
                    msleep(10);
                    continue;
                }
                if (r < 0) break;
                if (r == 0) break;
                string_append_bytes(&buf, tmp, (uint32_t)r);
                have += (uint32_t)r;
            }
        }

        if (have > 0) {
            char* body_copy = (char*)malloc(have);
            if (body_copy) {
                memcpy(body_copy, buf.data + body_start, have);
                req.body.ptr = (uintptr_t)body_copy;
                req.body.size = have;
            }
        }

        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_HTTP_SERVER;
        ev.action = NETLOG_ACT_HTTP_RECV_REQUEST;
        ev.pid = pid;
        ev.u0 = (uint32_t)req.method;
        ev.u1 = (uint32_t)req.path.length;
        ev.i0 = (int64_t)req.body.size;
        ev.local_port = client->get_local_port();
        ev.remote_ep = client->get_remote_ep();

        char pathbuf[128];
        if (req.path.length && req.path.data) {
            uint32_t n = req.path.length;
            if (n > sizeof(pathbuf) - 1) n = sizeof(pathbuf) - 1;
            memcpy(pathbuf, req.path.data, n);
            pathbuf[n] = 0;
            ev.s0 = pathbuf;
        }

        netlog_socket_event(&log_opts, &ev);

        free_sized(buf.data, buf.mem_length);
        return req;
    }

    int32_t send_response(TCPSocket* client, const HTTPResponseMsg& res) {
        if (!client) return SOCK_ERR_STATE;
        uint32_t code = (uint32_t)res.status_code;
        string out = http_response_builder(&res);
        uint32_t out_len = out.length;
        uint32_t off = 0;
        int64_t sent = 0;
        while (off < out_len) {
            int64_t r = client->send(out.data + off, out_len - off);
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
        ev.comp = NETLOG_COMP_HTTP_SERVER;
        ev.action = NETLOG_ACT_HTTP_SEND_RESPONSE;
        ev.pid = pid;
        ev.u0 = code;
        ev.u1 = out_len;
        ev.i0 = sent;
        ev.local_port = client->get_local_port();
        ev.remote_ep = client->get_remote_ep();
        netlog_socket_event(&log_opts, &ev);

        free_sized(out.data, out.mem_length);
        return sent < 0 ? (int32_t)sent : SOCK_OK;
    }

    int32_t close() {
        int32_t r = sock ? SOCK_OK : SOCK_ERR_STATE;
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_HTTP_SERVER;
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