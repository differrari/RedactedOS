#pragma once
#include "console/kio.h"
#include "networking/transport_layer/socket_tcp.hpp"
#include "http.h"
#include "std/std.h"
#include "net/socket_types.h"
#include "syscalls/syscalls.h"

struct HTTPConnection {
    TCPSocket* client;
    string carry_buf;
};

class HTTPServer {
private:
    uint16_t pid;
    TCPSocket* sock;
    SocketExtraOptions log_opts;
    SocketExtraOptions* tcp_extra;
    HTTPPolicy policy;

public:
    explicit HTTPServer(uint16_t pid_, const SocketExtraOptions* extra, const HTTPPolicyOptions* http_options) : pid(pid_), sock(nullptr), log_opts{}, tcp_extra(nullptr), policy(http_policy_from_options(http_options)) {
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
        if (sock) new (sock) TCPSocket(SOCK_ROLE_SERVER, pid, tcp_ptr);
    }

    ~HTTPServer() { close(); }

    int32_t set_options(const HTTPPolicyOptions* http_options) {
        policy = http_policy_from_options(http_options);
        return SOCK_OK;
    }

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

    HTTPConnection* accept() {
        TCPSocket* c = sock ? sock->accept() : nullptr;
        if (!c) return nullptr;

        HTTPConnection* conn = (HTTPConnection*)zalloc(sizeof(HTTPConnection));
        if (!conn) {
            delete c;
            return nullptr;
        }

        conn->client = c;
        conn->carry_buf = string{nullptr, 0, 0};

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
        return conn;
    }

    HTTPRequestMsg recv_request(HTTPConnection* conn) {
        HTTPRequestMsg req{};
        if (!conn || !conn->client) return req;

        TCPSocket* client = conn->client;
        string buf = string_repeat('\0', 0);
        if (conn->carry_buf.length) {
            string_append_bytes(&buf, conn->carry_buf.data, conn->carry_buf.length);
            string_free(conn->carry_buf);
            conn->carry_buf = string{nullptr, 0, 0};
        }

        char tmp[2048];
        int hdr_end = find_crlfcrlf(buf.data, buf.length);
        uint32_t consumed = 0;
        uint32_t start_ms = (uint32_t)get_time();
        uint32_t last_rx_ms = start_ms;
        bool bad_request = false;
        HttpError reject_status = HTTP_BAD_REQUEST;
        char* body_copy = nullptr;

        while (hdr_end < 0) {
            int64_t r = client->recv(tmp, sizeof(tmp));
            if (r == TCP_WOULDBLOCK) {
                uint32_t now = (uint32_t)get_time();
                if ((uint32_t)(now - last_rx_ms) > policy.header_idle_timeout_ms || (uint32_t)(now - start_ms) > policy.header_total_timeout_ms) {
                    string_free(buf);
                    return req;
                }
                msleep(10);
                continue;
            }
            if (r <= 0) {
                string_free(buf);
                return req;
            }
            string_append_bytes(&buf, tmp, (uint32_t)r);
            last_rx_ms = (uint32_t)get_time();
            if (buf.length > policy.max_header_bytes) {
                bad_request = true;
                reject_status = HTTP_HEADER_FIELDS_TOO_LARGE;
                break;
            }
            hdr_end = find_crlfcrlf(buf.data, buf.length);
        }

        if (!bad_request) {
            uint32_t line_end = 0;
            while (line_end + 1u < (uint32_t)hdr_end) {
                if (buf.data[line_end] == '\r' && buf.data[line_end + 1] == '\n') break;
                line_end++;
            }

            if (line_end > policy.max_start_line) {
                bad_request = true;
                reject_status = HTTP_URI_TOO_LONG;
            }

            HTTPRequestLine line{};
            HTTPParseResult line_result = HTTP_PARSE_OK;
            if (!bad_request) {
                line_result = http_parse_request_line(buf.data, line_end, &line);
                if (line_result != HTTP_PARSE_OK) {
                    bad_request = true;
                    reject_status = http_parse_result_status(line_result);
                }
            }

            if (!bad_request) {
                const char* target = buf.data + line.target_off;
                uint32_t target_len = line.target_len;
                req.method = line.method;
                req.version = line.version;
                req.path = string_repeat('\0', 0);

                if (target_len >= 7 && memcmp(target, "http://", 7) == 0) {
                    if (!policy.allow_absolute_uri) {
                        bad_request = true;
                        reject_status = HTTP_BAD_REQUEST;
                    } else {
                        uint32_t k = 7;
                        while (k < target_len && target[k] != '/') k++;
                        if (k < target_len) string_append_bytes(&req.path, target + k, target_len - k);
                        else string_append_bytes(&req.path, "/", 1);
                    }
                } else if (target_len >= 8 && memcmp(target, "https://", 8) == 0) {
                    if (!policy.allow_absolute_uri) {
                        bad_request = true;
                        reject_status = HTTP_BAD_REQUEST;
                    } else {
                        uint32_t k = 8;
                        while (k < target_len && target[k] != '/') k++;
                        if (k < target_len) string_append_bytes(&req.path, target + k, target_len - k);
                        else string_append_bytes(&req.path, "/", 1);
                    }
                } else string_append_bytes(&req.path, target, target_len);

                if (!bad_request && (!req.path.length || req.path.length > policy.max_path_len)) {
                    bad_request = true;
                    reject_status = req.path.length > policy.max_path_len ? HTTP_URI_TOO_LONG : HTTP_BAD_REQUEST;
                }
            }

            if (!bad_request) {
                HTTPParseResult header_result = http_header_parse_policy(
                    (char*)buf.data + line_end + 2,
                    (uint32_t)hdr_end - (line_end + 2),
                    &policy,
                    &req.headers_common,
                    &req.extra_headers,
                    &req.extra_header_count
                );

                if (header_result != HTTP_PARSE_OK) {
                    bad_request = true;
                    reject_status = http_parse_result_status(header_result);
                } else if (policy.require_host_http11 && line.version == HTTP_VERSION_11 && !req.headers_common.host.length) {
                    bad_request = true;
                    reject_status = HTTP_BAD_REQUEST;
                }
            }

            uint32_t body_start = hdr_end + 4;
            uint32_t have = buf.length > body_start ? buf.length - body_start : 0;
            uint32_t need = req.headers_common.has_length ? req.headers_common.length : 0;
            consumed = body_start;

            if (!bad_request && req.headers_common.chunked) {
                string chunk_buf = string_repeat('\0', 0);
                if (have) string_append_bytes(&chunk_buf, buf.data + body_start, have);

                uint32_t used = 0;
                string decoded = string_repeat('\0', 0);
                HTTPParseResult chunk_result = http_decode_chunked_body(chunk_buf.data, chunk_buf.length, &policy, &decoded, &used);
                uint32_t body_start_ms = (uint32_t)get_time();
                uint32_t body_last_rx_ms = body_start_ms;

                while (chunk_result == HTTP_PARSE_INCOMPLETE) {
                    int64_t r = client->recv(tmp, sizeof(tmp));
                    if (r == TCP_WOULDBLOCK) {
                        uint32_t now = (uint32_t)get_time();
                        if ((now - body_last_rx_ms) > policy.body_idle_timeout_ms || (now - body_start_ms) > policy.body_total_timeout_ms) break;
                        msleep(2);
                        continue;
                    }
                    if (r <= 0) break;
                    string_append_bytes(&chunk_buf, tmp, (uint32_t)r);
                    body_last_rx_ms = (uint32_t)get_time();
                    chunk_result = http_decode_chunked_body(chunk_buf.data, chunk_buf.length, &policy, &decoded, &used);
                }

                if (chunk_result != HTTP_PARSE_OK) {
                    if (decoded.mem_length) string_free(decoded);
                    bad_request = true;
                    reject_status = http_parse_result_status(chunk_result);
                } else {
                    consumed = buf.length;
                    if (used < chunk_buf.length) {
                        conn->carry_buf = string_repeat('\0', 0);
                        string_append_bytes(&conn->carry_buf, chunk_buf.data + used, chunk_buf.length - used);
                    }
                    if (decoded.length) {
                        req.body.ptr = (uintptr_t)decoded.data;
                        req.body.size = decoded.length;
                    } else if (decoded.mem_length) string_free(decoded);
                }
                string_free(chunk_buf);
            } else {
                if (!bad_request && need > policy.max_body_bytes) {
                    bad_request = true;
                    reject_status = HTTP_PAYLOAD_TOO_LARGE;
                }

                if (!bad_request && need > 0) {
                    body_copy = (char*)zalloc(need);
                    if (!body_copy) {
                        bad_request = true;
                        reject_status = HTTP_INTERNAL_SERVER_ERROR;
                    } else {
                        uint32_t copied = have < need ? have : need;
                        if (copied) memcpy(body_copy, buf.data + body_start, copied);

                        uint32_t body_start_ms = (uint32_t)get_time();
                        uint32_t body_last_rx_ms = body_start_ms;
                        while (copied < need) {
                            int64_t r = client->recv(body_copy + copied, need - copied);
                            if (r == TCP_WOULDBLOCK) {
                                uint32_t now = (uint32_t)get_time();
                                if ((now - body_last_rx_ms) > policy.body_idle_timeout_ms || (now - body_start_ms) > policy.body_total_timeout_ms) {
                                    bad_request = true;
                                    break;
                                }
                                msleep(2);
                                continue;
                            }
                            if (r <= 0) {
                                bad_request = true;
                                break;
                            }
                            copied += (uint32_t)r;
                            body_last_rx_ms = (uint32_t)get_time();
                        }
                    }
                }

                if (!bad_request) consumed = body_start + need;
                if (body_copy && !bad_request) {
                    req.body.ptr = (uintptr_t)body_copy;
                    req.body.size = need;
                }
            }
        }

        if (bad_request) {
            if (body_copy) release(body_copy);
            const char *reason = http_status_reason(reject_status);
            string body = string_format("%s\n", reason);
            HTTPResponseMsg res{};
            res.status_code = reject_status;
            res.reason = string_from_literal(reason);
            res.headers_common.length = body.length;
            res.headers_common.type = string_from_literal("text/plain");
            res.headers_common.connection = string_from_literal("close");
            res.body.ptr = (uintptr_t)body.data;
            res.body.size = body.length;
            send_response(conn, res);

            if (req.path.mem_length) string_free(req.path);
            http_headers_common_free(&req.headers_common);
            http_headers_extra_free(req.extra_headers, req.extra_header_count);
            http_headers_common_free(&res.headers_common);
            if (res.reason.mem_length) string_free(res.reason);
            string_free(body);
            string_free(buf);
            return HTTPRequestMsg{};
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

        if (consumed < buf.length && !conn->carry_buf.length) {
            conn->carry_buf = string_repeat('\0', 0);
            string_append_bytes(&conn->carry_buf, buf.data + consumed, buf.length - consumed);
        }

        string_free(buf);
        return req;
    }

    int32_t send_response(HTTPConnection* conn, const HTTPResponseMsg& res) {
        if (!conn || !conn->client) return SOCK_ERR_STATE;
        
        TCPSocket* client = conn->client;
        bool send_chunked = res.headers_common.chunked;
        HTTPResponseMsg head = res;
        if (!send_chunked) head.body = sizedptr{};
        uint32_t code = (uint32_t)res.status_code;
        string out = http_response_builder(&head);
        uint32_t body_len = (!send_chunked && res.body.ptr && res.body.size) ? (uint32_t)res.body.size : 0;
        uint32_t out_len = out.length + body_len;
        int64_t sent = 0;
        uint32_t start_ms = (uint32_t)get_time();
        uint32_t progress_ms = start_ms;
        const uint8_t* first_ptr = (const uint8_t*)out.data;
        uint32_t first_len = out.length;
        uint32_t first_body_len = 0;
        uint32_t body_off = 0;
        uint8_t* combo = nullptr;

        if (body_len && out.length < 1460) {
            first_body_len = body_len;
            uint32_t room = 1460 - out.length;
            if (first_body_len > room) first_body_len = room;

            if (first_body_len) {
                combo = (uint8_t*)zalloc(out.length + first_body_len);
                if (combo) {
                    memcpy(combo, out.data, out.length);
                    memcpy(combo + out.length, (const void*)res.body.ptr, first_body_len);

                    first_ptr = combo;
                    first_len = out.length + first_body_len;
                } else first_body_len = 0;
            }
        }
        
        uint32_t off = 0;
        while (sent >= 0 && off < first_len) {
            int64_t r = client->send(first_ptr + off, first_len - off);
            uint32_t now = (uint32_t)get_time();

            if (r == TCP_WOULDBLOCK || r == 0) {
                if ((now - progress_ms) > 3000 || (now - start_ms) > 30000) {
                    sent = SOCK_ERR_SYS;
                    break;
                }
                msleep(2);
                continue;
            }
            if (r < 0) {
                sent = r;
                break;
            }
            off += (uint32_t)r;
            progress_ms = now;
        }

        if (sent >= 0) body_off = first_body_len;
        if (sent >= 0 && body_len) {
            const uint8_t* body = (const uint8_t*)res.body.ptr;

            while (body_off < body_len) {
                uint32_t ask = body_len - body_off;
                if (ask > 16384) ask = 16384;
                int64_t r = client->send(body + body_off, ask);
                uint32_t now = (uint32_t)get_time();

                if (r == TCP_WOULDBLOCK || r == 0) {
                    if ((now - progress_ms) > 3000 || (now - start_ms) > 30000) {
                        sent = SOCK_ERR_SYS;
                        break;
                    }
                    msleep(2);
                    continue;
                }
                if (r < 0) {
                    sent = r;
                    break;
                }
                body_off += (uint32_t)r;
                progress_ms = now;
            }
        }
        
        if (sent >= 0) sent = (int64_t)out.length + body_off;
        if (combo) release(combo);
        
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

        string_free(out);
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
        if (sock) release(sock);
        sock = nullptr;

        if (tcp_extra) release(tcp_extra);
        tcp_extra = nullptr;

        log_opts.flags &= ~SOCK_OPT_DEBUG;
        return r;
    }
};