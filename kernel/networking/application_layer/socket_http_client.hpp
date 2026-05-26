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
    HTTPPolicy policy;

public:
    explicit HTTPClient(uint16_t pid_, const SocketExtraOptions* extra, const HTTPPolicyOptions* http_options) : pid(pid_), sock(nullptr), log_opts{}, tcp_extra(nullptr), policy(http_policy_from_options(http_options)) {
        if (extra) log_opts = *extra;

        const SocketExtraOptions* tcp_ptr = extra;
        if (extra && (log_opts.flags & SOCK_OPT_DEBUG)) {
            tcp_extra = (SocketExtraOptions*)zalloc(sizeof(SocketExtraOptions));
            if (tcp_extra) {
                *tcp_extra = *extra;
                tcp_extra->flags &= ~SOCK_OPT_DEBUG;
                tcp_ptr = tcp_extra;
            }
        }

        sock = (TCPSocket*)zalloc(sizeof(TCPSocket));
        if (sock) new (sock) TCPSocket(SOCK_ROLE_CLIENT, pid, tcp_ptr);
    }

    ~HTTPClient() {close();}

    int32_t set_options(const HTTPPolicyOptions* http_options) {
        policy = http_policy_from_options(http_options);
        return SOCK_OK;
    }

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
        string_free(out);

        if (sent < 0) {
            resp.status_code = (HttpError)sent;
            return resp;
        }

        string buf = string_repeat('\0', 0);
        char tmp[512];
        int hdr_end = -1;
        uint32_t start_ms = (uint32_t)get_time();
        uint32_t last_rx_ms = start_ms;

        while (hdr_end < 0) {
            int64_t r = sock->recv(tmp, sizeof(tmp));
            if (r == TCP_WOULDBLOCK) {
                uint32_t now = (uint32_t)get_time();
                if ((now - last_rx_ms) > policy.header_idle_timeout_ms || (now - start_ms) > policy.header_total_timeout_ms) {
                    string_free(buf);
                    resp.status_code = (HttpError)SOCK_ERR_PROTO;
                    return resp;
                }
                msleep(10);
                continue;
            }
            if (r < 0) {
                string_free(buf);
                resp.status_code = (HttpError)r;
                return resp;
            }
            if (r == 0) {
                string_free(buf);
                resp.status_code = (HttpError)SOCK_ERR_PROTO;
                return resp;
            }
            string_append_bytes(&buf, tmp, (uint32_t)r);
            last_rx_ms = (uint32_t)get_time();
            if (buf.length > policy.max_header_bytes) {
                string_free(buf);
                resp.status_code = (HttpError)SOCK_ERR_PROTO;
                return resp;
            }
            hdr_end = find_crlfcrlf(buf.data, buf.length);
        }

        int status_line_end = strindex((char*)buf.data, "\r\n");
        if (status_line_end <= 0) {
            string_free(buf);
            resp.status_code = (HttpError)SOCK_ERR_PROTO;
            return resp;
        }

        HTTPStatusLine status_line{};
        if (http_parse_status_line(buf.data, status_line_end, &status_line) != HTTP_PARSE_OK) {
            string_free(buf);
            resp.status_code = (HttpError)SOCK_ERR_PROTO;
            return resp;
        }

        resp.status_code = (HttpError)status_line.status_code;
        if (status_line.reason_len) {
            resp.reason = string_repeat('\0', 0);
            string_append_bytes(&resp.reason, buf.data + status_line.reason_off, status_line.reason_len);
        }

        HTTPHeader *extras = nullptr;
        uint32_t extra_count = 0;
        HTTPParseResult header_result = http_header_parse_policy(
            (char*)buf.data + status_line_end + 2,
            (uint32_t)hdr_end - (uint32_t)(status_line_end + 2),
            &policy,
            &resp.headers_common,
            &extras,
            &extra_count);
        resp.extra_headers = extras;
        resp.extra_header_count = extra_count;

        if (header_result != HTTP_PARSE_OK) {
            if (resp.reason.mem_length) string_free(resp.reason);
            resp.reason = string{};
            http_headers_common_free(&resp.headers_common);
            http_headers_extra_free(resp.extra_headers, resp.extra_header_count);
            resp.extra_headers = nullptr;
            resp.extra_header_count = 0;
            string_free(buf);
            sock->close();
            resp.status_code = (HttpError)SOCK_ERR_PROTO;
            return resp;
        }

        uint32_t body_start = (uint32_t)hdr_end + 4;
        uint32_t have = buf.length > body_start ? buf.length - body_start : 0;

        if (resp.headers_common.chunked) {
            string chunk_buf = string_repeat('\0', 0);
            if (have) string_append_bytes(&chunk_buf, buf.data + body_start, have);

            uint32_t used = 0;
            string decoded = string{};
            HTTPParseResult chunk_result = http_decode_chunked_body(chunk_buf.data, chunk_buf.length, &policy, &decoded, &used);
            uint32_t body_start_ms = (uint32_t)get_time();
            uint32_t body_last_rx_ms = body_start_ms;

            while (chunk_result == HTTP_PARSE_INCOMPLETE) {
                int64_t r = sock->recv(tmp, sizeof(tmp));
                if (r == TCP_WOULDBLOCK) {
                    uint32_t now = (uint32_t)get_time();
                    if ((now - body_last_rx_ms) > policy.body_idle_timeout_ms || (now - body_start_ms) > policy.body_total_timeout_ms) break;
                    msleep(1);
                    continue;
                }
                if (r <= 0) break;
                string_append_bytes(&chunk_buf, tmp, (uint32_t)r);
                body_last_rx_ms = (uint32_t)get_time();
                chunk_result = http_decode_chunked_body(chunk_buf.data, chunk_buf.length, &policy, &decoded, &used);
            }

            string_free(chunk_buf);
            if (chunk_result != HTTP_PARSE_OK) {
                if (decoded.mem_length) string_free(decoded);
                if (resp.reason.mem_length) string_free(resp.reason);
                resp.reason = string{};
                http_headers_common_free(&resp.headers_common);
                http_headers_extra_free(resp.extra_headers, resp.extra_header_count);
                resp.extra_headers = nullptr;
                resp.extra_header_count = 0;
                string_free(buf);
                sock->close();
                resp.status_code = (HttpError)SOCK_ERR_PROTO;
                return resp;
            }
            if (decoded.length) {
                resp.body.ptr = (uintptr_t)decoded.data;
                resp.body.size = decoded.length;
            } else if (decoded.mem_length) string_free(decoded);
        } else {
            uint32_t need = resp.headers_common.has_length ? resp.headers_common.length : 0;
            uint32_t body_len = resp.headers_common.has_length ? need : have;
            char *body_copy = nullptr;

            if (body_len > policy.max_body_bytes) {
                if (resp.reason.mem_length) string_free(resp.reason);
                resp.reason = string{};
                http_headers_common_free(&resp.headers_common);
                http_headers_extra_free(resp.extra_headers, resp.extra_header_count);
                resp.extra_headers = nullptr;
                resp.extra_header_count = 0;
                string_free(buf);
                sock->close();
                resp.status_code = (HttpError)SOCK_ERR_PROTO;
                return resp;
            }

            if (body_len > 0) {
                body_copy = (char*)zalloc(body_len);
                if (!body_copy) {
                    if (resp.reason.mem_length) string_free(resp.reason);
                    resp.reason = string{};
                    http_headers_common_free(&resp.headers_common);
                    http_headers_extra_free(resp.extra_headers, resp.extra_header_count);
                    resp.extra_headers = nullptr;
                    resp.extra_header_count = 0;
                    string_free(buf);
                    sock->close();
                    resp.status_code = (HttpError)SOCK_ERR_SYS;
                    return resp;
                }

                uint32_t copied = have < body_len ? have : body_len;
                if (copied) memcpy(body_copy, buf.data + body_start, copied);

                if (resp.headers_common.has_length) {
                    uint32_t body_start_ms = (uint32_t)get_time();
                    uint32_t body_last_rx_ms = body_start_ms;
                    while (copied < need) {
                        int64_t r = sock->recv(body_copy + copied, need - copied);
                        if (r == TCP_WOULDBLOCK) {
                            uint32_t now = (uint32_t)get_time();
                            if ((now - body_last_rx_ms) > policy.body_idle_timeout_ms || (now - body_start_ms) > policy.body_total_timeout_ms) break;
                            msleep(1);
                            continue;
                        }
                        if (r <= 0) break;
                        copied += (uint32_t)r;
                        body_last_rx_ms = (uint32_t)get_time();
                    }

                    if (copied < need) {
                        release(body_copy);
                        if (resp.reason.mem_length) string_free(resp.reason);
                        resp.reason = string{};
                        http_headers_common_free(&resp.headers_common);
                        http_headers_extra_free(resp.extra_headers, resp.extra_header_count);
                        resp.extra_headers = nullptr;
                        resp.extra_header_count = 0;
                        string_free(buf);
                        sock->close();
                        resp.status_code = (HttpError)SOCK_ERR_PROTO;
                        return resp;
                    }
                }

                resp.body.ptr = (uintptr_t)body_copy;
                resp.body.size = body_len;
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

        string_free(buf);
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
        if (sock) release(sock);
        sock = nullptr;

        if (tcp_extra) release(tcp_extra);
        tcp_extra = nullptr;

        log_opts.flags &= ~SOCK_OPT_DEBUG;
        return r;
    }
};
