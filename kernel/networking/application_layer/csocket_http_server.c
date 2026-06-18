#include "csocket_http_server.h"
#include "console/kio.h"
#include "networking/transport_layer/csocket.h"
#include "networking/net_logger/net_logger.h"
#include "http.h"
#include "std/std.h"
#include "net/socket_types.h"
#include "syscalls/syscalls.h"
#include "process/scheduler.h"
#include "alloc/allocate.h"

typedef struct HTTPConnection {
    socket_handle_t client;
    string carry_buf;
    uint32_t request_count;
    HTTPMethod current_method;
    bool close_after_response;
    bool send_keep_alive_header;
} HTTPConnection;

typedef struct HTTPServer {
    socket_handle_t sock;
    SocketOptions log_opts;
    SocketOptions tcp_opts;
    HTTPServerPolicy policy;
} HTTPServer;

http_server_handle_t http_server_create(const SocketOptions* extra, const HTTPServerPolicyOptions* http_options) {
    HTTPServer* srv = (HTTPServer*)zalloc(sizeof(*srv));
    if (!srv) return NULL;

    srv->policy = http_server_policy_from_options(http_options);
    if (extra) {
        srv->log_opts = *extra;
        srv->tcp_opts = *extra;
        srv->tcp_opts.flags &= ~SOCK_OPT_DEBUG;
    }

    return srv;
}

void http_server_destroy(http_server_handle_t h) {
    if (!h) return;
    HTTPServer* srv = (HTTPServer*)h;
    http_server_close(srv);
    release(srv);
}

int32_t http_server_set_options(http_server_handle_t h, const HTTPServerPolicyOptions* http_options) {
    if (!h) return (int32_t)SOCK_ERR_INVAL;
    HTTPServer* srv = (HTTPServer*)h;
    srv->policy = http_server_policy_from_options(http_options);
    return SOCK_OK;
}

int32_t http_server_bind(http_server_handle_t h, const SockBindSpec* spec, uint16_t port) {
    if (!h || !spec) return (int32_t)SOCK_ERR_INVAL;

    HTTPServer* srv = (HTTPServer*)h;
    uint16_t p = port;
    if (!srv->sock) srv->sock = create_socket(PROTO_TCP, &srv->tcp_opts);
    if (!srv->sock) return SOCK_ERR_SYS;
    int32_t r = bind_socket(srv->sock, spec, p);

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_HTTP_SERVER;
    ev.action = NETLOG_ACT_BIND;
    ev.pid = get_current_proc_pid();
    ev.u0 = p;
    ev.i0 = r;
    netlog_socket_event(&srv->log_opts, &ev);
    return r;
}

int32_t http_server_listen(http_server_handle_t h, int backlog) {
    if (!h) return (int32_t)SOCK_ERR_INVAL;

    HTTPServer* srv = (HTTPServer*)h;
    int b = backlog;
    int32_t r = srv->sock ? listen_on(srv->sock, b) : SOCK_ERR_STATE;

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_HTTP_SERVER;
    ev.action = NETLOG_ACT_LISTEN;
    ev.pid = get_current_proc_pid();
    ev.u0 = (uint32_t)b;
    ev.i0 = r;
    netlog_socket_event(&srv->log_opts, &ev);
    return r;
}

http_connection_handle_t http_server_accept(http_server_handle_t h) {
    if (!h) return NULL;

    HTTPServer* srv = (HTTPServer*)h;
    if (!srv->sock) return NULL;
    socket_handle_t child = accept_on_socket(srv->sock);
    if (!child) return NULL;

    HTTPConnection* conn = (HTTPConnection*)zalloc(sizeof(HTTPConnection));
    if (!conn) {
        close_socket(child);
        return NULL;
    }

    conn->client = child;
    conn->carry_buf = (string){0};
    conn->request_count = 0;
    conn->current_method = HTTP_METHOD_GET;
    conn->close_after_response = true;
    conn->send_keep_alive_header = false;

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_HTTP_SERVER;
    ev.action = NETLOG_ACT_ACCEPT;
    ev.pid = get_current_proc_pid();
    ev.i0 = (int64_t)child;
    uint32_t local_port = 0;
    uint32_t opt_len = sizeof(local_port);
    if (get_socket_option(child, SOCK_GET_LOCAL_PORT, &local_port, &opt_len) == SOCK_OK) ev.local_port = local_port;
    opt_len = sizeof(ev.remote_ep);
    get_socket_option(child, SOCK_GET_REMOTE_ENDPOINT, &ev.remote_ep, &opt_len);
    netlog_socket_event(&srv->log_opts, &ev);
    return conn;
}

HTTPRequestMsg http_server_recv_request(http_server_handle_t h, http_connection_handle_t c) {
    HTTPRequestMsg req = {0};
    if (!h || !c) return req;

    HTTPServer* srv = (HTTPServer*)h;
    HTTPConnection* conn = (HTTPConnection*)c;
    if (!conn->client) return req;

    string buf = (string){0};
    if (conn->carry_buf.length) {
        string_append_bytes(&buf, conn->carry_buf.data, conn->carry_buf.length);
        string_free(conn->carry_buf);
        conn->carry_buf = (string){0};
    }

    char tmp[2048];
    int hdr_end = find_crlfcrlf(buf.data, buf.length);
    uint32_t consumed = 0;
    uint32_t start_ms = (uint32_t)get_time();
    uint32_t last_rx_ms = start_ms;
    bool bad_request = false;
    HttpError reject_status = HTTP_BAD_REQUEST;
    char* body_copy = NULL;

    while (hdr_end < 0) {
        int64_t r = receive_from_socket(conn->client, tmp, sizeof(tmp), NULL);
        if (r == SOCK_ERR_WOULDBLOCK) {
            uint32_t now = (uint32_t)get_time();
            if ((uint32_t)(now - last_rx_ms) > srv->policy.common.header_idle_timeout_ms || (uint32_t)(now - start_ms) > srv->policy.common.header_total_timeout_ms) {
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
        if (buf.length > srv->policy.common.max_header_bytes) {
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

        if (line_end > srv->policy.common.max_start_line) {
            bad_request = true;
            reject_status = HTTP_URI_TOO_LONG;
        }

        HTTPRequestLine line = {0};
        HTTPParseResult line_result = HTTP_PARSE_OK;
        if (!bad_request) {
            line_result = http_parse_request_line(buf.data, line_end, &line);
            if (line_result != HTTP_PARSE_OK) {
                bad_request = true;
                reject_status = http_parse_result_status(line_result);
            } else if (!http_method_allowed(srv->policy.allowed_methods, line.method)) {
                bad_request = true;
                reject_status = HTTP_METHOD_NOT_ALLOWED;
            }
        }

        if (!bad_request) {
            const char* target = buf.data + line.target_off;
            uint32_t target_len = line.target_len;
            req.method = line.method;
            req.version = line.version;
            req.path = (string){0};

            if (target_len >= 7 && memcmp(target, "http://", 7) == 0) {
                if (!srv->policy.allow_absolute_uri) {
                    bad_request = true;
                    reject_status = HTTP_BAD_REQUEST;
                } else {
                    uint32_t k = 7;
                    while (k < target_len && target[k] != '/') k++;
                    if (k < target_len) string_append_bytes(&req.path, target + k, target_len - k);
                    else string_append_bytes(&req.path, "/", 1);
                }
            } else if (target_len >= 8 && memcmp(target, "https://", 8) == 0) {
                if (!srv->policy.allow_absolute_uri) {
                    bad_request = true;
                    reject_status = HTTP_BAD_REQUEST;
                } else {
                    uint32_t k = 8;
                    while (k < target_len && target[k] != '/') k++;
                    if (k < target_len) string_append_bytes(&req.path, target + k, target_len - k);
                    else string_append_bytes(&req.path, "/", 1);
                }
            } else string_append_bytes(&req.path, target, target_len);

            if (!bad_request && (!req.path.length || req.path.length > srv->policy.common.max_path_len)) {
                bad_request = true;
                reject_status = req.path.length > srv->policy.common.max_path_len ? HTTP_URI_TOO_LONG : HTTP_BAD_REQUEST;
            }
        }

        if (!bad_request) {
            HTTPParseResult header_result = http_header_parse(
                (char*)buf.data + line_end + 2,
                (uint32_t)hdr_end - (line_end + 2),
                &srv->policy.common,
                &req.headers_common,
                &req.extra_headers,
                &req.extra_header_count);

            if (header_result != HTTP_PARSE_OK) {
                bad_request = true;
                reject_status = http_parse_result_status(header_result);
            } else if (srv->policy.require_host_http11 && line.version == HTTP_VERSION_11 && !req.headers_common.fields.host.length) {
                bad_request = true;
                reject_status = HTTP_BAD_REQUEST;
            } else {
                conn->request_count++;
                conn->current_method = req.method;
                conn->close_after_response = true;
                conn->send_keep_alive_header = false;
                if (srv->policy.allow_keep_alive) {
                    if (line.version == HTTP_VERSION_11) conn->close_after_response = req.headers_common.framing.connection_close != 0;
                    else if (line.version == HTTP_VERSION_10 && req.headers_common.framing.connection_keep_alive) {
                        conn->close_after_response = false;
                        conn->send_keep_alive_header = true;
                    }
                }
                if (srv->policy.max_keepalive_requests && conn->request_count >= srv->policy.max_keepalive_requests) conn->close_after_response = true;
            }
        }

        uint32_t body_start = hdr_end + 4;
        uint32_t have = buf.length > body_start ? buf.length - body_start : 0;

        if (!bad_request && req.headers_common.framing.expect_continue) {
            if (req.headers_common.framing.has_content_length && req.headers_common.fields.content_length > srv->policy.common.max_body_bytes) {
                bad_request = true;
                reject_status = HTTP_PAYLOAD_TOO_LARGE;
            } else {
                HTTPResponseMsg cont = {0};
                cont.status_code = HTTP_CONTINUE;
                http_server_send_response(srv, conn, &cont);
            }
        }
        uint32_t need = req.headers_common.framing.has_content_length ? req.headers_common.fields.content_length : 0;
        consumed = body_start;

        if (!bad_request && req.headers_common.framing.chunked) {
            HTTPChunkedDecoder dec;
            http_chunked_decoder_init(&dec, &srv->policy.common);

            uint32_t used = 0;
            HTTPParseResult chunk_result = have ? http_chunked_decoder_feed(&dec, buf.data + body_start, have, &used) : HTTP_PARSE_INCOMPLETE;
            uint32_t body_start_ms = (uint32_t)get_time();
            uint32_t body_last_rx_ms = body_start_ms;
            consumed = body_start + used;

            while (chunk_result == HTTP_PARSE_INCOMPLETE) {
                int64_t r = receive_from_socket(conn->client, tmp, sizeof(tmp), NULL);
                if (r == SOCK_ERR_WOULDBLOCK) {
                    uint32_t now = (uint32_t)get_time();
                    if ((now - body_last_rx_ms) > srv->policy.common.body_idle_timeout_ms || (now - body_start_ms) > srv->policy.common.body_total_timeout_ms) break;
                    msleep(2);
                    continue;
                }
                if (r <= 0) break;
                used = 0;
                chunk_result = http_chunked_decoder_feed(&dec, tmp, (uint32_t)r, &used);
                body_last_rx_ms = (uint32_t)get_time();
                consumed = buf.length;
                if (chunk_result == HTTP_PARSE_OK && used < (uint32_t)r && !conn->carry_buf.length) {
                    conn->carry_buf = (string){0};
                    string_append_bytes(&conn->carry_buf, tmp + used, (uint32_t)r - used);
                }
            }

            if (chunk_result != HTTP_PARSE_OK) {
                bad_request = true;
                reject_status = http_parse_result_status(chunk_result);
            } else {
                string decoded = dec.body;
                dec.body = (string){0};
                if (decoded.length) {
                    req.body = decoded;
                } else if (decoded.mem_length) string_free(decoded);
            }
            http_chunked_decoder_free(&dec);
        } else {
            if (!bad_request && need > srv->policy.common.max_body_bytes) {
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
                        int64_t r = receive_from_socket(conn->client, body_copy + copied, need - copied, NULL);
                        if (r == SOCK_ERR_WOULDBLOCK) {
                            uint32_t now = (uint32_t)get_time();
                            if ((now - body_last_rx_ms) > srv->policy.common.body_idle_timeout_ms || (now - body_start_ms) > srv->policy.common.body_total_timeout_ms) {
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
                req.body = (string){body_copy, need, need};
            }
        }
    }

    if (bad_request) {
        if (body_copy) release(body_copy);
        const char* reason = http_status_reason(reject_status);
        string body = srv->policy.send_error_body ? string_format("%s\n", reason) : (string){0};
        HTTPResponseMsg res = {0};
        HTTPHeader allow_header = {0};
        string allow_value = {0};
        static char allow_key[] = "Allow";

        res.status_code = reject_status;
        res.headers_common.fields.content_length = body.length;
        res.headers_common.framing.has_content_length = 1;
        if (srv->policy.error_content_type) res.headers_common.fields.content_type = string_from_literal(srv->policy.error_content_type);
        res.body = body;

        if (reject_status == HTTP_METHOD_NOT_ALLOWED) {
            allow_value = http_methods_allow_header(srv->policy.allowed_methods);
            allow_header.key = (string){allow_key, sizeof(allow_key) - 1, 0};
            allow_header.value = allow_value;
            res.extra_headers = &allow_header;
            res.extra_header_count = 1;
        }

        conn->close_after_response = true;
        http_server_send_response(srv, conn, &res);
        if (conn->client) {
            close_socket(conn->client);
            conn->client = 0;
        }

        http_request_free(&req);
        http_headers_common_free(&res.headers_common);
        if (allow_value.mem_length) string_free(allow_value);
        if (body.mem_length) string_free(body);
        string_free(buf);
        return (HTTPRequestMsg){0};
    }

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_HTTP_SERVER;
    ev.action = NETLOG_ACT_HTTP_RECV_REQUEST;
    ev.pid = get_current_proc_pid();
    ev.u0 = (uint32_t)req.method;
    ev.u1 = (uint32_t)req.path.length;
    ev.i0 = (int64_t)req.body.length;
    uint32_t local_port = 0;
    uint32_t opt_len = sizeof(local_port);
    if (get_socket_option(conn->client, SOCK_GET_LOCAL_PORT, &local_port, &opt_len) == SOCK_OK) ev.local_port = local_port;
    opt_len = sizeof(ev.remote_ep);
    get_socket_option(conn->client, SOCK_GET_REMOTE_ENDPOINT, &ev.remote_ep, &opt_len);

    char pathbuf[128];
    if (req.path.length && req.path.data) {
        uint32_t n = req.path.length;
        if (n > sizeof(pathbuf) - 1) n = sizeof(pathbuf) - 1;
        memcpy(pathbuf, req.path.data, n);
        pathbuf[n] = 0;
        ev.s0 = pathbuf;
    }

    netlog_socket_event(&srv->log_opts, &ev);

    if (consumed < buf.length && !conn->carry_buf.length) {
        conn->carry_buf = (string){0};
        string_append_bytes(&conn->carry_buf, buf.data + consumed, buf.length - consumed);
    }

    string_free(buf);
    return req;
}

int32_t http_server_send_response(http_server_handle_t h, http_connection_handle_t c, const HTTPResponseMsg* res) {
    if (!h || !c || !res) return (int32_t)SOCK_ERR_INVAL;

    HTTPServer* srv = (HTTPServer*)h;
    HTTPConnection* conn = (HTTPConnection*)c;
    if (!conn->client) return SOCK_ERR_STATE;

    uint32_t code = (uint32_t)res->status_code;
    bool informational = code >= 100 && code < 200;
    bool suppress_body = informational || conn->current_method == HTTP_METHOD_HEAD || code == 204 || code == 304;
    bool send_chunked = suppress_body ? false : res->headers_common.framing.chunked;
    HTTPResponseMsg head = *res;
    bool explicit_close = http_header_value_has_token(res->headers_common.fields.connection.data, res->headers_common.fields.connection.length, "close", 5);
    bool explicit_keep_alive = http_header_value_has_token(res->headers_common.fields.connection.data, res->headers_common.fields.connection.length, "keep-alive", 10);
    bool close_after_send = !informational && (conn->close_after_response || explicit_close);
    static char conn_close_data[] = "close";
    static char conn_keep_alive_data[] = "keep-alive";
    const string conn_close = {conn_close_data, sizeof(conn_close_data) - 1, 0};
    const string conn_keep_alive = {conn_keep_alive_data, sizeof(conn_keep_alive_data) - 1, 0};

    if (!informational) {
        if (close_after_send) head.headers_common.fields.connection = conn_close;
        else if (!res->headers_common.fields.connection.length && conn->send_keep_alive_header) head.headers_common.fields.connection = conn_keep_alive;
        else if (explicit_keep_alive) head.headers_common.fields.connection = res->headers_common.fields.connection;
    }

    if (suppress_body) {
        head.body = (string){0};
        head.headers_common.framing.chunked = 0;
    }

    uint32_t body_len = (!send_chunked && !suppress_body && res->body.data && res->body.length) ? (uint32_t)res->body.length : 0;
    if (!send_chunked) {
        head.body = (string){0};
        if (body_len && !head.headers_common.framing.has_content_length) {
            head.headers_common.fields.content_length = body_len;
            head.headers_common.framing.has_content_length = 1;
        }
    }
    string out = http_response_builder(&head);
    uint32_t out_len = out.length + body_len;
    int64_t sent = 0;
    uint32_t start_ms = (uint32_t)get_time();
    uint32_t progress_ms = start_ms;
    const uint8_t* first_ptr = (const uint8_t*)out.data;
    uint32_t first_len = out.length;
    uint32_t first_body_len = 0;
    uint32_t body_off = 0;
    uint8_t* combo = NULL;

    if (body_len && out.length < 1460) {
        first_body_len = body_len;
        uint32_t room = 1460 - out.length;
        if (first_body_len > room) first_body_len = room;

        if (first_body_len) {
            combo = (uint8_t*)zalloc(out.length + first_body_len);
            if (combo) {
                memcpy(combo, out.data, out.length);
                memcpy(combo + out.length, (const void*)res->body.data, first_body_len);

                first_ptr = combo;
                first_len = out.length + first_body_len;
            } else first_body_len = 0;
        }
    }

    uint32_t off = 0;
    while (sent >= 0 && off < first_len) {
        int64_t r = send_on_socket(conn->client, (void*)(first_ptr + off), first_len - off);
        uint32_t now = (uint32_t)get_time();

        if (r == SOCK_ERR_WOULDBLOCK || r == 0) {
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
        const uint8_t* body = (const uint8_t*)res->body.data;

        while (body_off < body_len) {
            uint32_t ask = body_len - body_off;
            if (ask > 16384) ask = 16384;
            int64_t r = send_on_socket(conn->client, (void*)(body + body_off), ask);
            uint32_t now = (uint32_t)get_time();

            if (r == SOCK_ERR_WOULDBLOCK || r == 0) {
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

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_HTTP_SERVER;
    ev.action = NETLOG_ACT_HTTP_SEND_RESPONSE;
    ev.pid = get_current_proc_pid();
    ev.u0 = code;
    ev.u1 = out_len;
    ev.i0 = sent;
    uint32_t local_port = 0;
    uint32_t opt_len = sizeof(local_port);
    if (get_socket_option(conn->client, SOCK_GET_LOCAL_PORT, &local_port, &opt_len) == SOCK_OK) ev.local_port = local_port;
    opt_len = sizeof(ev.remote_ep);
    get_socket_option(conn->client, SOCK_GET_REMOTE_ENDPOINT, &ev.remote_ep, &opt_len);
    netlog_socket_event(&srv->log_opts, &ev);

    string_free(out);
    if (sent >= 0 && close_after_send && conn->client) {
        close_socket(conn->client);
        conn->client = 0;
    }
    return sent < 0 ? (int32_t)sent : SOCK_OK;
}

int32_t http_connection_close(http_connection_handle_t c) {
    if (!c) return (int32_t)SOCK_ERR_INVAL;
    HTTPConnection* conn = (HTTPConnection*)c;
    if (conn->carry_buf.mem_length) string_free(conn->carry_buf);
    if (conn->client) close_socket(conn->client);
    release(conn);
    return (int32_t)SOCK_OK;
}

int32_t http_server_close(http_server_handle_t h) {
    if (!h) return (int32_t)SOCK_ERR_INVAL;

    HTTPServer* srv = (HTTPServer*)h;
    int32_t r = srv->sock ? SOCK_OK : SOCK_ERR_STATE;
    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_HTTP_SERVER;
    ev.action = NETLOG_ACT_CLOSE;
    ev.pid = get_current_proc_pid();
    ev.i0 = r;
    if (srv->sock) {
        uint32_t local_port = 0;
        uint32_t opt_len = sizeof(local_port);
        if (get_socket_option(srv->sock, SOCK_GET_LOCAL_PORT, &local_port, &opt_len) == SOCK_OK) ev.local_port = local_port;
        opt_len = sizeof(ev.remote_ep);
        get_socket_option(srv->sock, SOCK_GET_REMOTE_ENDPOINT, &ev.remote_ep, &opt_len);
    }
    netlog_socket_event(&srv->log_opts, &ev);

    if (srv->sock) {
        r = close_socket(srv->sock);
        srv->sock = 0;
    }

    return r;
}
