#pragma once
#include "console/kio.h"
#include "net/transport_layer/socket_tcp.hpp"
#include "http.h"
#include "std/std.h"
#include "net/transport_layer/socket_types.h"

class HTTPServer {
private:
    TCPSocket* sock;
    bool http_log;
    SocketExtraOptions* tcp_extra;

public:
    explicit HTTPServer(uint16_t pid, const SocketExtraOptions* extra) : sock(nullptr), http_log(false), tcp_extra(nullptr) {
        uint32_t flags = 0;
        if (extra) flags = extra->flags;
        http_log = (flags & SOCK_OPT_DEBUG) != 0;

        const SocketExtraOptions* tcp_ptr = extra;
        if (http_log && extra) {
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
        if (http_log) kprintf("[HTTP] server bind port=%u r=%d", (uint32_t)p, (int32_t)r);
        return r;
    }

    int32_t listen(int backlog = 4) {
        int b = backlog;
        int32_t r = sock ? sock->listen(b) : SOCK_ERR_STATE;
        if (http_log) kprintf("[HTTP] server listen backlog=%d r=%d", (int32_t)b, (int32_t)r);
        return r;
    }

    TCPSocket* accept() {
        TCPSocket* c = sock ? sock->accept() : nullptr;
        if (http_log && c) kprintf("[HTTP] server accept client=%p", c);
        return c;
    }

    HTTPRequestMsg recv_request(TCPSocket* client) {
        HTTPRequestMsg req{};
        if (!client) return req;

        string buf = string_repeat('\0', 0);
        char tmp[512];
        int hdr_end = -1;
        int attempts = 0;
        const int max_attempts = 500;

        while (true) {
            int64_t r = client->recv(tmp, sizeof(tmp));
            if (r < 0) {
                if (http_log) kprintf("[HTTP] server recv_request recv fail=%lld", (long long)r);
                free(buf.data, buf.mem_length);
                return req;
            }
            if (r > 0) string_append_bytes(&buf, tmp, (uint32_t)r);
            hdr_end = find_crlfcrlf(buf.data, buf.length);
            if (hdr_end >= 0) break;
            if (++attempts > max_attempts) {
                if (http_log) kprintf("[HTTP] server recv_request timeout");
                free(buf.data, buf.mem_length);
                return req;
            }
            sleep(10);
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
        string method_tok = string_repeat('\0', 0);
        string_append_bytes(&method_tok, buf.data + p, i - p);

        if (method_tok.length == 3 && memcmp(method_tok.data, "GET", 3) == 0) req.method = HTTP_METHOD_GET;
        else if (method_tok.length == 4 && memcmp(method_tok.data, "POST", 4) == 0) req.method = HTTP_METHOD_POST;
        else if (method_tok.length == 3 && memcmp(method_tok.data, "PUT", 3) == 0) req.method = HTTP_METHOD_PUT;
        else if (method_tok.length == 6 && memcmp(method_tok.data, "DELETE", 6) == 0) req.method = HTTP_METHOD_DELETE;
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
                free(req.path.data, req.path.mem_length);
                req.path = newp;
            }
        } else if (req.path.length >= 8 && memcmp(req.path.data, "https://", 8) == 0) {
            uint32_t k = 8;
            while (k < req.path.length && req.path.data[k] != '/') ++k;
            if (k < req.path.length) {
                string newp = string_repeat('\0', 0);
                string_append_bytes(&newp, req.path.data + k, req.path.length - k);
                free(req.path.data, req.path.mem_length);
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
                if (r <= 0) break;
                string_append_bytes(&buf, tmp, (uint32_t)r);
                have += (uint32_t)r;
            }
        } else {
            int idle = 0;
            while (idle < 5) {
                int64_t r = client->recv(tmp, sizeof(tmp));
                if (r > 0) {
                    string_append_bytes(&buf, tmp, (uint32_t)r);
                    have += (uint32_t)r;
                    idle = 0;
                } else {
                    ++idle;
                    sleep(20);
                }
            }
        }

        if (have > 0) {
            char* body_copy = (char*)malloc(have + 1);
            if (body_copy) {
                memcpy(body_copy, buf.data + body_start, have);
                body_copy[have] = '\0';
                req.body.ptr = (uintptr_t)body_copy;
                req.body.size = have;
            }
        }

        if (http_log) kprintf("[HTTP] server recv_request method=%u path_len=%u body=%u", (uint32_t)req.method, (uint32_t)req.path.length, (uint32_t)req.body.size);
        free(method_tok.data, method_tok.mem_length);
        free(buf.data, buf.mem_length);
        return req;
    }

    int32_t send_response(TCPSocket* client, const HTTPResponseMsg& res) {
        if (!client) return SOCK_ERR_STATE;
        uint32_t code = (uint32_t)res.status_code;
        string out = http_response_builder(&res);
        uint32_t out_len = out.length;
        int64_t sent = client->send(out.data, out.length);
        if (http_log) kprintf("[HTTP] server send_response code=%u bytes=%u sent=%lld", code, (uint32_t)out_len, (long long)sent);
        free(out.data, out.mem_length);
        return sent < 0 ? (int32_t)sent : SOCK_OK;
    }

    int32_t close() {
        int32_t r = SOCK_ERR_STATE;

        if (sock) r = sock->close();
        if (http_log) kprintf("[HTTP] server close r=%d", (int32_t)r);

        if (sock) sock->~TCPSocket();
        if (sock) free(sock, sizeof(TCPSocket));
        sock = nullptr;

        if (tcp_extra) free(tcp_extra, sizeof(SocketExtraOptions));
        tcp_extra = nullptr;

        http_log = false;
        return r;
    }
};