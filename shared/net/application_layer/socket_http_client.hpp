#pragma once
#include "console/kio.h"
#include "net/transport_layer/socket_tcp.hpp"
#include "http.h"
#include "std/std.h"
#include "net/transport_layer/socket_types.h"

class HTTPClient {
private:
    TCPSocket* sock;
    bool http_log;
    SocketExtraOptions* tcp_extra;

public:
    explicit HTTPClient(uint16_t pid, const SocketExtraOptions* extra) : sock(nullptr), http_log(false), tcp_extra(nullptr) {
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
        if (sock) new (sock) TCPSocket(SOCK_ROLE_CLIENT, pid, tcp_ptr);
    }

    ~HTTPClient() {close();}

    int32_t connect(SockDstKind kind, const void* dst, uint16_t port) {
        uint16_t p = port;
        int32_t r = sock ? sock->connect(kind, dst, p) : SOCK_ERR_STATE;
        if (http_log) kprintf("[HTTP] client connect port=%u r=%d", (uint32_t)p, (int32_t)r);
        return r;
    }

    HTTPResponseMsg send_request(const HTTPRequestMsg &req) {
        HTTPResponseMsg resp{};
        if (!sock) {
            resp.status_code = (HttpError)SOCK_ERR_STATE;
            return resp;
        }
        
        string out = http_request_builder(&req);
        uint32_t out_len = out.length;
        int64_t sent = sock->send(out.data, out.length);
        if (http_log) kprintf("[HTTP] client send_request bytes=%u sent=%lld", (uint32_t)out_len, (long long)sent);
        free(out.data, out.mem_length);

        if (sent < 0) {
            resp.status_code = (HttpError)sent;
            if (http_log) kprintf("[HTTP] client send_request fail=%lld", (long long)sent);
            return resp;
        }

        string buf = string_repeat('\0', 0);
        char tmp[512];
        int attempts = 0;
        int hdr_end = -1;
        while (true) {
            int64_t r = sock->recv(tmp, sizeof(tmp));
            if (r < 0) {
                if (http_log) kprintf("[HTTP] client recv hdr fail=%lld", (long long)r);
                free(buf.data, buf.mem_length);
                resp.status_code = (HttpError)SOCK_ERR_SYS;
                return resp;
            }
            if (r > 0) string_append_bytes(&buf, tmp, (uint32_t)r);
            hdr_end = find_crlfcrlf(buf.data, buf.length);
            if (hdr_end >= 0) break;
            if (++attempts > 50) {
                if (http_log) kprintf("[HTTP] client recv hdr timeout");
                free(buf.data, buf.mem_length);
                resp.status_code = (HttpError)SOCK_ERR_PROTO;
                return resp;
            }
            sleep(10);
        }

        {
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
                if (r <= 0) break;
                string_append_bytes(&buf, tmp, (uint32_t)r);
                have += (uint32_t)r;
            }
        } else {
            int idle = 0;
            while (idle < 5) {
                int64_t r = sock->recv(tmp, sizeof(tmp));
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
            char *body_copy = (char*)malloc(have + 1);
            if (body_copy) {
                memcpy(body_copy, buf.data + body_start, have);
                body_copy[have] = '\0';
                resp.body.ptr = (uintptr_t)body_copy;
                resp.body.size = have;
            }
        }

        if (http_log) kprintf("[HTTP] client recv_response code=%u body=%u", (uint32_t)resp.status_code, (uint32_t)resp.body.size);
        free(buf.data, buf.mem_length);
        return resp;
    }

    int32_t close() {
        int32_t r = SOCK_ERR_STATE;

        if (sock) r = sock->close();
        if (http_log) kprintf("[HTTP] client close r=%d", (int32_t)r);

        if (sock) sock->~TCPSocket();
        if (sock) free(sock, sizeof(TCPSocket));
        sock = nullptr;

        if (tcp_extra) free(tcp_extra, sizeof(SocketExtraOptions));
        tcp_extra = nullptr;

        http_log = false;
        return r;
    }
};
