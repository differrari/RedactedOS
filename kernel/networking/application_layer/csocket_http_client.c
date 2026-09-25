#include "csocket_http_client.h"
#include "csocket_http_internal.h"
#include "console/kio.h"
#include "networking/transport_layer/csocket.h"
#include "networking/net_logger/net_logger.h"
#include "http.h"
#include "std/std.h"
#include "net/socket_types.h"
#include "data/format/url.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/transport_layer/socket_endpoint.h"
#include "process/scheduler.h"
#include "alloc/allocate.h"

typedef struct HTTPClientOrigin {
    string domain;
    net_l4_endpoint ep;
    bool valid;
} HTTPClientOrigin;

typedef struct HTTPClient {
    socket_handle_t sock;
    SocketOptions log_opts;
    SocketOptions tcp_opts;
    HTTPClientPolicy policy;
    HTTPClientOrigin origin;
} HTTPClient;

static HTTPResponseMsg http_client_error_response(int64_t err) {
    HTTPResponseMsg resp = {0};
    resp.status_code = (HttpError)err;
    return resp;
}

static HTTPResponseMsg http_client_receive_response(HTTPClient* cli, HTTPMethod request_method) {
    HTTPResponseMsg resp = {0};
    string buf = (string){0};
    char tmp[512];

    while (1) {
        int32_t hdr_end = -1;
        int32_t head_result = http_socket_recv_head(cli->sock, &buf, cli->policy.common.max_header_bytes, cli->policy.common.header_idle_timeout_ms, cli->policy.common.header_total_timeout_ms, &hdr_end);
        if (head_result < 0) {
            string_free(buf);
            return http_client_error_response(head_result == SOCK_ERR_WOULDBLOCK ? SOCK_ERR_PROTO : head_result);
        }

        int32_t status_line_end = strindex((char*)buf.data, "\r\n");
        if (status_line_end <= 0) {
            string_free(buf);
            return http_client_error_response(SOCK_ERR_PROTO);
        }

        HTTPStatusLine status_line = {0};
        if (http_parse_status_line(buf.data, status_line_end, &status_line) != HTTP_PARSE_OK) {
            string_free(buf);
            return http_client_error_response(SOCK_ERR_PROTO);
        }

        resp.status_code = (HttpError)status_line.status_code;
        if (status_line.reason_len) {
            resp.reason = (string){0};
            string_append_bytes(&resp.reason, buf.data + status_line.reason_off, status_line.reason_len);
        }

        HTTPParseResult header_result = http_header_parse(
            (char*)buf.data + status_line_end + 2,
            (uint32_t)hdr_end - (uint32_t)(status_line_end + 2),
            &cli->policy.common,
            &resp.headers_common,
            &resp.extra_headers,
            &resp.extra_header_count);

        if (header_result != HTTP_PARSE_OK) {
            http_response_free(&resp);
            string_free(buf);
            if (cli->sock) (void)http_socket_close(&cli->sock, true);
            return http_client_error_response(SOCK_ERR_PROTO);
        }

        uint32_t body_start = (uint32_t)hdr_end + 4;
        uint32_t have = buf.length > body_start ? buf.length - body_start : 0;
        uint32_t code = (uint32_t)resp.status_code;
        bool no_body = request_method == HTTP_METHOD_HEAD || code == 204 || code == 304;
        if (code >= 100 && code < 200) {
            HTTPResponseMsg info = resp;
            resp = (HTTPResponseMsg){0};
            if (info.reason.mem_length) string_free(info.reason);
            http_headers_common_free(&info.headers_common);
            http_headers_extra_free(info.extra_headers, info.extra_header_count);

            string next = (string){0};
            if (have) string_append_bytes(&next, buf.data + body_start, have);
            string_free(buf);
            buf = next;
            continue;
        }

        if (no_body) {
        } else if (resp.headers_common.framing.chunked) {
            HTTPChunkedDecoder dec;
            http_chunked_decoder_init(&dec, &cli->policy.common);
            uint32_t used = 0;
            HTTPParseResult chunk_result = have ? http_chunked_decoder_feed(&dec, buf.data + body_start, have, &used) : HTTP_PARSE_INCOMPLETE;
            uint32_t body_start_ms = (uint32_t)get_time();
            HTTPSocketTimeoutState body_timeout = {body_start_ms, body_start_ms};

            while (chunk_result == HTTP_PARSE_INCOMPLETE) {
                int64_t r = http_socket_recv_wait(cli->sock, tmp, sizeof(tmp), cli->policy.common.body_idle_timeout_ms, cli->policy.common.body_total_timeout_ms, &body_timeout);
                if (r <= 0) break;
                chunk_result = http_chunked_decoder_feed(&dec, tmp, (uint32_t)r, &used);
            }

            if (chunk_result != HTTP_PARSE_OK) {
                http_chunked_decoder_free(&dec);
                http_response_free(&resp);
                string_free(buf);
                if (cli->sock) (void)http_socket_close(&cli->sock, true);
                return http_client_error_response(SOCK_ERR_PROTO);
            }

            string decoded = dec.body;
            dec.body = (string){0};
            http_chunked_decoder_free(&dec);
            if (decoded.length) {
                resp.body = decoded;
            } else if (decoded.mem_length) string_free(decoded);
        } else {
            uint32_t need = resp.headers_common.framing.has_content_length ? resp.headers_common.fields.content_length : 0;

            if (resp.headers_common.framing.has_content_length) {
                if (need > cli->policy.common.max_body_bytes) {
                    http_response_free(&resp);
                    string_free(buf);
                    if (cli->sock) (void)http_socket_close(&cli->sock, true);
                    return http_client_error_response(SOCK_ERR_PROTO);
                }

                if (need) {
                    char* body_copy = (char*)zalloc(need);
                    if (!body_copy) {
                        http_response_free(&resp);
                        string_free(buf);
                        if (cli->sock) (void)http_socket_close(&cli->sock, true);
                        return http_client_error_response(SOCK_ERR_SYS);
                    }

                    uint32_t copied = have < need ? have : need;
                    if (copied) memcpy(body_copy, buf.data + body_start, copied);

                    if (copied < need) {
                        int64_t receive_result = http_socket_recv_exact(cli->sock, body_copy + copied, need - copied, cli->policy.common.body_idle_timeout_ms, cli->policy.common.body_total_timeout_ms);
                        if (receive_result < 0) {
                            release(body_copy);
                            http_response_free(&resp);
                            string_free(buf);
                            if (cli->sock) (void)http_socket_close(&cli->sock, true);
                            return http_client_error_response(receive_result == SOCK_ERR_WOULDBLOCK ? SOCK_ERR_PROTO : receive_result);
                        }
                    }

                    resp.body = (string){body_copy, need, need};
                }
            } else if (cli->policy.allow_close_delimited) {
                string body = (string){0};
                if (have) string_append_bytes(&body, buf.data + body_start, have);

                uint32_t body_start_ms = (uint32_t)get_time();
                HTTPSocketTimeoutState body_timeout = {body_start_ms, body_start_ms};
                while (body.length <= cli->policy.common.max_body_bytes) {
                    int64_t r = http_socket_recv_wait(cli->sock, tmp, sizeof(tmp), cli->policy.common.body_idle_timeout_ms, cli->policy.common.body_total_timeout_ms, &body_timeout);
                    if (r < 0) {
                        string_free(body);
                        http_response_free(&resp);
                        string_free(buf);
                        if (cli->sock) (void)http_socket_close(&cli->sock, true);
                        return http_client_error_response(r == SOCK_ERR_WOULDBLOCK ? SOCK_ERR_PROTO : r);
                    }
                    if (r == 0) break;
                    if (body.length + (uint32_t)r > cli->policy.common.max_body_bytes) {
                        string_free(body);
                        http_response_free(&resp);
                        string_free(buf);
                        if (cli->sock) (void)http_socket_close(&cli->sock, true);
                        return http_client_error_response(SOCK_ERR_PROTO);
                    }
                    string_append_bytes(&body, tmp, (uint32_t)r);
                }

                if (body.length) {
                    resp.body = body;
                } else if (body.mem_length) string_free(body);
            }
        }

        netlog_socket_event_t ev = {0};
        ev.comp = NETLOG_COMP_HTTP_CLIENT;
        ev.action = NETLOG_ACT_HTTP_RECV_RESPONSE;
        ev.pid = get_current_proc_pid();
        ev.u0 = (uint32_t)resp.status_code;
        ev.u1 = (uint32_t)resp.body.length;
        http_socket_fill_log_endpoints(cli->sock, &ev);
        netlog_socket_event(&cli->log_opts, &ev);

        string_free(buf);
        return resp;
    }
}

static int32_t http_client_connect_endpoint_origin(HTTPClient* cli, const net_l4_endpoint* dst, const char* host_name) {
    if (!cli->sock) {
        SocketOptions connect_options = cli->tcp_opts;
        connect_options.flags &= ~SOCK_OPT_NONBLOCK;
        cli->sock = create_socket(PROTO_TCP, &connect_options);
    }
    if (!cli->sock) return SOCK_ERR_SYS;
    int32_t r = connect_socket(cli->sock, dst);
    if (r >= 0 && (r = set_socket_option(cli->sock, SOCK_OPT_NONBLOCK, &(uint32_t){1}, sizeof(uint32_t))) >= 0) {
        HTTPClientOrigin next = {0};
        next.valid = true;
        if (dst) next.ep = *dst;
        if (host_name) next.domain = string_from_literal(host_name);
        if (cli->origin.domain.mem_length) string_free(cli->origin.domain);
        cli->origin = next;
    } else {
        (void)http_socket_close(&cli->sock, true);
    }

    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_HTTP_CLIENT;
    ev.action = NETLOG_ACT_CONNECT;
    ev.pid = get_current_proc_pid();
    if (dst) ev.dst_ep = *dst;
    ev.i0 = r;

    if (cli->sock) {
        http_socket_fill_log_endpoints(cli->sock, &ev);
        if (ev.remote_ep.ver) ev.dst_ep = ev.remote_ep;
    }

    netlog_socket_event(&cli->log_opts, &ev);
    return r;
}

static int32_t http_client_connect_domain_origin(HTTPClient* cli, const char* host, uint16_t port) {
    if (!host || !port) return SOCK_ERR_INVAL;
    net_l4_endpoint ep = socket_endpoint_select(host, port, (ip_version_t)0, DNS_USE_BOTH, 3000);
    if (!ep.ver) return SOCK_ERR_DNS;
    return http_client_connect_endpoint_origin(cli, &ep, host);
}

http_client_handle_t http_client_create(const SocketOptions* extra, const HTTPClientPolicyOptions* http_options) {
    HTTPClient* cli = (HTTPClient*)zalloc(sizeof(*cli));
    if (!cli) return NULL;

    cli->policy = http_client_policy_from_options(http_options);
    if (extra) {
        cli->log_opts = *extra;
        cli->tcp_opts = *extra;
        cli->tcp_opts.flags &= ~SOCK_OPT_DEBUG;
    }

    return cli;
}

void http_client_destroy(http_client_handle_t h) {
    if (!h) return;
    HTTPClient* cli = (HTTPClient*)h;
    if (cli->sock) (void)http_socket_close(&cli->sock, true);
    if (cli->origin.domain.mem_length) string_free(cli->origin.domain);
    release(cli);
}

int32_t http_client_set_options(http_client_handle_t h, const HTTPClientPolicyOptions* http_options) {
    if (!h) return (int32_t)SOCK_ERR_INVAL;
    HTTPClient* cli = (HTTPClient*)h;
    cli->policy = http_client_policy_from_options(http_options);
    return SOCK_OK;
}

int32_t http_client_connect_endpoint(http_client_handle_t h, const net_l4_endpoint* dst) {
    if (!h || !dst) return (int32_t)SOCK_ERR_INVAL;
    HTTPClient* cli = (HTTPClient*)h;
    return http_client_connect_endpoint_origin(cli, dst, NULL);
}

int32_t http_client_connect_domain(http_client_handle_t h, const char* host, uint16_t port) {
    if (!h || !host) return (int32_t)SOCK_ERR_INVAL;
    HTTPClient* cli = (HTTPClient*)h;
    return http_client_connect_domain_origin(cli, host, port);
}

HTTPResponseMsg http_client_send_request(http_client_handle_t h, const HTTPRequestMsg* req) {
    HTTPResponseMsg empty = {0};
    if (!h || !req) {
        empty.status_code = (HttpError)SOCK_ERR_INVAL;
        return empty;
    }

    HTTPClient* cli = (HTTPClient*)h;
    HTTPRequestMsg curr = *req;
    curr.path = (string){0};
    if (req->path.data && req->path.length) string_append_bytes(&curr.path, req->path.data, req->path.length);
    curr.headers_common.fields.host = (string){0};
    if (req->headers_common.fields.host.data && req->headers_common.fields.host.length) string_append_bytes(&curr.headers_common.fields.host, req->headers_common.fields.host.data, req->headers_common.fields.host.length);

    HTTPResponseMsg resp = {0};
    uint32_t redirects = cli->policy.follow_redirects ? cli->policy.max_redirects : 0;
    for (uint32_t i = 0;; i++) {
        if (!cli->sock) {
            resp = http_client_error_response(SOCK_ERR_STATE);
            break;
        }

        if (!curr.host_override && curr.version != HTTP_VERSION_10 && !curr.headers_common.fields.host.length && cli->origin.valid) {
            if (cli->origin.domain.data) {
                uint16_t host_port = cli->origin.ep.port;
                bool ipv6 = str_has_char(cli->origin.domain.data, cli->origin.domain.length, ':') >= 0;
                if (host_port && host_port != 80) {
                    if (ipv6) curr.headers_common.fields.host = string_format("[%.*s]:%i", (int)cli->origin.domain.length, cli->origin.domain.data, (int)host_port);
                    else curr.headers_common.fields.host = string_format("%.*s:%i", (int)cli->origin.domain.length, cli->origin.domain.data, (int)host_port);
                } else {
                    if (ipv6) curr.headers_common.fields.host = string_format("[%.*s]", (int)cli->origin.domain.length, cli->origin.domain.data);
                    else curr.headers_common.fields.host = string_from_literal_length(cli->origin.domain.data, cli->origin.domain.length);
                }
            } else if (cli->origin.ep.ver) {
                char ip[48];
                bool ipv6 = false;
                uint16_t ep_port = 0;
                net_ep_split(&cli->origin.ep, ip, sizeof(ip), &ipv6, &ep_port);
                uint16_t host_port = ep_port;
                if (host_port && host_port != 80) {
                    if (ipv6) curr.headers_common.fields.host = string_format("[%s]:%i", ip, (int)host_port);
                    else curr.headers_common.fields.host = string_format("%s:%i", ip, (int)host_port);
                } else {
                    if (ipv6) curr.headers_common.fields.host = string_format("[%s]", ip);
                    else curr.headers_common.fields.host = string_from_literal(ip);
                }
            }
        }

        string out = http_request_builder(&curr);
        uint32_t out_len = out.length;

        uint32_t send_start_ms = (uint32_t)get_time();
        HTTPSocketTimeoutState send_timeout = {send_start_ms, send_start_ms};
        int64_t sent = http_socket_send_all(cli->sock, out.data, out_len, 0, HTTP_SOCKET_WRITE_IDLE_TIMEOUT_MS, HTTP_SOCKET_WRITE_TOTAL_TIMEOUT_MS, &send_timeout);

        netlog_socket_event_t ev = {0};
        ev.comp = NETLOG_COMP_HTTP_CLIENT;
        ev.action = NETLOG_ACT_HTTP_SEND_REQUEST;
        ev.pid = get_current_proc_pid();
        ev.u0 = out_len;
        ev.i0 = sent;
        http_socket_fill_log_endpoints(cli->sock, &ev);

        char pathbuf[128];
        if (curr.path.length && curr.path.data) {
            uint32_t n = curr.path.length;
            if (n > sizeof(pathbuf) - 1) n = sizeof(pathbuf) - 1;
            memcpy(pathbuf, curr.path.data, n);
            pathbuf[n] = 0;
            ev.s0 = pathbuf;
        }

        netlog_socket_event(&cli->log_opts, &ev);
        string_free(out);

        if (sent < 0) {
            resp = http_client_error_response(sent);
            break;
        }

        resp = http_client_receive_response(cli, curr.method);
        uint32_t code = (uint32_t)resp.status_code;
        bool redirect = code == HTTP_MOVED_PERMANENTLY || code == HTTP_FOUND || code == HTTP_SEE_OTHER || code == HTTP_TEMPORARY_REDIRECT || code == HTTP_PERMANENT_REDIRECT;
        if (!redirect || i >= redirects || !resp.headers_common.fields.location.length) break;

        ParsedURL url = parse_url(resp.headers_common.fields.location.data, resp.headers_common.fields.location.length);
        if (!url.ok) break;

        bool absolute = url.scheme.size || url.host.size;
        if (url.scheme.size) {
            if (url.scheme.size == 5 && strncmp_case((const char*)url.scheme.ptr, "https", true, 5) == 0) break;
            if (!(url.scheme.size == 4 && strncmp_case((const char*)url.scheme.ptr, "http", true, 4) == 0)) break;
        }
        if (absolute && (!url.host.ptr || !url.host.size)) break;

        string next_path = url_request_path(&url, &curr.path);
        if (!next_path.length) break;

        if (curr.path.mem_length) string_free(curr.path);
        curr.path = next_path;

        if (absolute) {
            uint16_t next_port = url.port ? url.port : 80;
            string next_domain = string_from_literal_length((const char*)url.host.ptr, url.host.size);
            if (!curr.host_override) {
                if (curr.headers_common.fields.host.mem_length) string_free(curr.headers_common.fields.host);
                curr.headers_common.fields.host = (string){0};
            }

            int32_t rr = cli->sock ? http_socket_close(&cli->sock, false) : SOCK_OK;
            if (rr >= 0) rr = next_domain.data ? http_client_connect_domain_origin(cli, next_domain.data, next_port) : SOCK_ERR_SYS;
            if (next_domain.mem_length) string_free(next_domain);
            if (rr < 0) {
                http_response_free(&resp);
                resp = http_client_error_response(rr);
                break;
            }
        } else {
            net_l4_endpoint reconnect_ep = cli->origin.ep;
            string reconnect_domain = {0};
            if (cli->origin.domain.data) reconnect_domain = string_from_literal(cli->origin.domain.data);

            int32_t rr = cli->sock ? http_socket_close(&cli->sock, false) : SOCK_OK;
            if (rr >= 0 && cli->origin.valid) {
                if (reconnect_domain.data) rr = http_client_connect_domain_origin(cli, reconnect_domain.data, reconnect_ep.port);
                else rr = http_client_connect_endpoint_origin(cli, &reconnect_ep, NULL);
            } else if (rr >= 0) rr = SOCK_ERR_STATE;
            if (reconnect_domain.mem_length) string_free(reconnect_domain);
            if (rr < 0) {
                http_response_free(&resp);
                resp = http_client_error_response(rr);
                break;
            }
        }

        if (code == HTTP_SEE_OTHER && curr.method != HTTP_METHOD_HEAD) {
            curr.method = HTTP_METHOD_GET;
            curr.body = (string){0};
            curr.headers_common.framing.chunked = 0;
            curr.headers_common.framing.has_content_length = 0;
            curr.headers_common.fields.content_length = 0;
        }

        http_response_free(&resp);
    }

    if (curr.path.mem_length) string_free(curr.path);
    if (curr.headers_common.fields.host.mem_length) string_free(curr.headers_common.fields.host);
    return resp;
}

int32_t http_client_close(http_client_handle_t h) {
    if (!h) return (int32_t)SOCK_ERR_INVAL;

    HTTPClient* cli = (HTTPClient*)h;
    netlog_socket_event_t ev = {0};
    ev.comp = NETLOG_COMP_HTTP_CLIENT;
    ev.action = NETLOG_ACT_CLOSE;
    ev.pid = get_current_proc_pid();

    if (cli->sock) http_socket_fill_log_endpoints(cli->sock, &ev);

    int32_t r = cli->sock ? http_socket_close(&cli->sock, false) : SOCK_ERR_STATE;
    ev.i0 = r;
    netlog_socket_event(&cli->log_opts, &ev);

    if (!cli->sock){
        if (cli->origin.domain.mem_length) string_free(cli->origin.domain);
        cli->origin = (HTTPClientOrigin){0};
    }
    return r;
}
