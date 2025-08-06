#pragma once

#include "console/kio.h"
#include "net/transport_layer/socket_tcp.hpp"
#include "http.h"
#include "std/string.h"
#include "std/memfunctions.h"

#define KP(fmt, ...) \
    do { kprintf(fmt, ##__VA_ARGS__); } while (0)

class HTTPServer {
private:
    TCPSocket sock;

public:
    explicit HTTPServer(uint16_t pid) : sock(SOCK_ROLE_SERVER, pid) {}

    ~HTTPServer() { close(); }

    int32_t bind(uint16_t port) { return sock.bind(port); }
    int32_t listen(int backlog = 4) { return sock.listen(backlog); }
    TCPSocket* accept() { return sock.accept(); }

    HTTPRequestMsg recv_request(TCPSocket* client) {
        HTTPRequestMsg req{};
        if (!client) return req;

        string buf = string_repeat('\0', 0);
        char tmp[512];
        int attempts = 0;
        int hdr_end  = -1;

        while (true) {
            int64_t r = client->recv(tmp, sizeof(tmp));
            if (r < 0) return req;
            if (r > 0) string_append_bytes(&buf, tmp, (uint32_t)r);
            hdr_end = find_crlfcrlf(buf.data, buf.length);
            if (hdr_end >= 0) break;
            if (++attempts > 100) return req;
            sleep(10);
        }

        uint32_t i = 0;
        while (i < (uint32_t)hdr_end && buf.data[i] != ' ') ++i;
        string method_tok = string_repeat('\0', 0);
        string_append_bytes(&method_tok, buf.data, i);

        if (method_tok.length == 3 && memcmp(method_tok.data, "GET",    3) == 0)
            req.method = HTTP_METHOD_GET;
        else if (method_tok.length == 4 && memcmp(method_tok.data, "POST",   4) == 0)
            req.method = HTTP_METHOD_POST;
        else if (method_tok.length == 3 && memcmp(method_tok.data, "PUT",    3) == 0)
            req.method = HTTP_METHOD_PUT;
        else if (method_tok.length == 6 && memcmp(method_tok.data, "DELETE", 6) == 0)
            req.method = HTTP_METHOD_DELETE;
        else
            req.method = HTTP_METHOD_GET;

        uint32_t j = i + 1;
        uint32_t path_start = j;
        while (j < (uint32_t)hdr_end && buf.data[j] != ' ') ++j;
        req.path = string_repeat('\0', 0);
        string_append_bytes(&req.path, buf.data + path_start, j - path_start);

        int status_line_end = strindex((char*)buf.data, "\r\n");
        http_header_parser(
            (char*)buf.data + status_line_end + 2,
            buf.length - (uint32_t)(status_line_end + 2),
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
                req.body.ptr   = (uintptr_t)body_copy;
                req.body.size  = have;
            }
        }

        free(buf.data, buf.mem_length);
        return req;
    }


    int32_t send_response(TCPSocket* client, const HTTPResponseMsg& res) {
        if (!client) return SOCK_ERR_STATE;
        string out = http_response_builder(&res);
        int64_t sent = client->send(out.data, out.length);
        free(out.data, out.mem_length);
        return sent < 0 ? (int32_t)sent : SOCK_OK;
    }

    int32_t close() { return sock.close(); }
};
