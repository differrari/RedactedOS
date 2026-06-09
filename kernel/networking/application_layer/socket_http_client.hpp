#pragma once
#include "console/kio.h"
#include "networking/transport_layer/csocket.h"
#include "networking/transport_layer/tcp.h"
#include "networking/net_logger/net_logger.h"
#include "http.h"
#include "std/std.h"
#include "net/socket_types.h"
#include "data/format/url.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/transport_layer/socket_endpoint.h"
#include "process/scheduler.h"

struct HTTPClientOrigin {
    string domain;
    net_l4_endpoint ep;
    bool valid;
};

class HTTPClient {
private:
    socket_handle_t sock;
    SocketExtraOptions log_opts;
    SocketExtraOptions* tcp_extra;
    HTTPClientPolicy policy;
    HTTPClientOrigin origin;

    void free_response(HTTPResponseMsg *resp) {
        if (!resp) return;
        if (resp->body.ptr && resp->body.size) release((void*)resp->body.ptr);
        if (resp->reason.mem_length) string_free(resp->reason);
        http_headers_common_free(&resp->headers_common);
        http_headers_extra_free(resp->extra_headers, resp->extra_header_count);
        *resp = HTTPResponseMsg{};
    }

    HTTPResponseMsg error_response(int64_t err) {
        HTTPResponseMsg resp{};
        resp.status_code = (HttpError)err;
        return resp;
    }

    HTTPResponseMsg receive_response(HTTPMethod request_method) {
        HTTPResponseMsg resp{};
        string buf = string_repeat('\0', 0);
        char tmp[512];

        while (1) {
            int hdr_end = find_crlfcrlf(buf.data, buf.length);
            uint32_t start_ms = (uint32_t)get_time();
            uint32_t last_rx_ms = start_ms;

            while (hdr_end < 0) {
                int64_t r = receive_from_socket(sock, tmp, sizeof(tmp), nullptr);
                if (r == TCP_WOULDBLOCK) {
                    uint32_t now = (uint32_t)get_time();
                    if ((now - last_rx_ms) > policy.common.header_idle_timeout_ms || (now - start_ms) > policy.common.header_total_timeout_ms) {
                        string_free(buf);
                        return error_response(SOCK_ERR_PROTO);
                    }
                    msleep(10);
                    continue;
                }
                if (r < 0) {
                    string_free(buf);
                    return error_response(r);
                }
                if (r == 0) {
                    string_free(buf);
                    return error_response(SOCK_ERR_PROTO);
                }
                string_append_bytes(&buf, tmp, (uint32_t)r);
                last_rx_ms = (uint32_t)get_time();
                if (buf.length > policy.common.max_header_bytes) {
                    string_free(buf);
                    return error_response(SOCK_ERR_PROTO);
                }
                hdr_end = find_crlfcrlf(buf.data, buf.length);
            }

            int status_line_end = strindex((char*)buf.data, "\r\n");
            if (status_line_end <= 0) {
                string_free(buf);
                return error_response(SOCK_ERR_PROTO);
            }

            HTTPStatusLine status_line{};
            if (http_parse_status_line(buf.data, status_line_end, &status_line) != HTTP_PARSE_OK) {
                string_free(buf);
                return error_response(SOCK_ERR_PROTO);
            }

            resp.status_code = (HttpError)status_line.status_code;
            if (status_line.reason_len) {
                resp.reason = string_repeat('\0', 0);
                string_append_bytes(&resp.reason, buf.data + status_line.reason_off, status_line.reason_len);
            }

            HTTPParseResult header_result = http_header_parse(
                (char*)buf.data + status_line_end + 2,
                (uint32_t)hdr_end - (uint32_t)(status_line_end + 2),
                &policy.common,
                &resp.headers_common,
                &resp.extra_headers,
                &resp.extra_header_count);

            if (header_result != HTTP_PARSE_OK) {
                free_response(&resp);
                string_free(buf);
                if (sock) {
                    close_socket(sock);
                    sock = 0;
                }
                return error_response(SOCK_ERR_PROTO);
            }

            uint32_t body_start = (uint32_t)hdr_end + 4;
            uint32_t have = buf.length > body_start ? buf.length - body_start : 0;
            uint32_t code = (uint32_t)resp.status_code;
            bool no_body = request_method == HTTP_METHOD_HEAD || code == 204 || code == 304;
            if (code >= 100 && code < 200) {
                HTTPResponseMsg info = resp;
                resp = HTTPResponseMsg{};
                if (info.reason.mem_length) string_free(info.reason);
                http_headers_common_free(&info.headers_common);
                http_headers_extra_free(info.extra_headers, info.extra_header_count);

                string next = string_repeat('\0', 0);
                if (have) string_append_bytes(&next, buf.data + body_start, have);
                string_free(buf);
                buf = next;
                continue;
            }

            if (no_body) { 
            } else if (resp.headers_common.framing.chunked) {
                HTTPChunkedDecoder dec;
                http_chunked_decoder_init(&dec, &policy.common);
                uint32_t used = 0;
                HTTPParseResult chunk_result = have ? http_chunked_decoder_feed(&dec, buf.data + body_start, have, &used) : HTTP_PARSE_INCOMPLETE;
                uint32_t body_start_ms = (uint32_t)get_time();
                uint32_t body_last_rx_ms = body_start_ms;

                while (chunk_result == HTTP_PARSE_INCOMPLETE) {
                    int64_t r = receive_from_socket(sock, tmp, sizeof(tmp), nullptr);
                    if (r == TCP_WOULDBLOCK) {
                        uint32_t now = (uint32_t)get_time();
                        if ((now - body_last_rx_ms) > policy.common.body_idle_timeout_ms || (now - body_start_ms) > policy.common.body_total_timeout_ms) break;
                        msleep(1);
                        continue;
                    }
                    if (r <= 0) break;
                    chunk_result = http_chunked_decoder_feed(&dec, tmp, (uint32_t)r, &used);
                    body_last_rx_ms = (uint32_t)get_time();
                }

                if (chunk_result != HTTP_PARSE_OK) {
                    http_chunked_decoder_free(&dec);
                    free_response(&resp);
                    string_free(buf);
                    if (sock) {
                        close_socket(sock);
                        sock = 0;
                    }
                    return error_response(SOCK_ERR_PROTO);
                }

                string decoded = dec.body;
                dec.body = string{};
                http_chunked_decoder_free(&dec);
                if (decoded.length) {
                    resp.body.ptr = (uintptr_t)decoded.data;
                    resp.body.size = decoded.length;
                } else if (decoded.mem_length) string_free(decoded);
            } else {
                uint32_t need = resp.headers_common.framing.has_content_length ? resp.headers_common.fields.content_length : 0;

                if (resp.headers_common.framing.has_content_length) {
                    if (need > policy.common.max_body_bytes) {
                        free_response(&resp);
                        string_free(buf);
                        if (sock) {
                            close_socket(sock);
                            sock = 0;
                        }
                        return error_response(SOCK_ERR_PROTO);
                    }

                    if (need) {
                        char *body_copy = (char*)zalloc(need);
                        if (!body_copy) {
                            free_response(&resp);
                            string_free(buf);
                            if (sock) {
                                close_socket(sock);
                                sock = 0;
                            }
                            return error_response(SOCK_ERR_SYS);
                        }

                        uint32_t copied = have < need ? have : need;
                        if (copied) memcpy(body_copy, buf.data + body_start, copied);

                        uint32_t body_start_ms = (uint32_t)get_time();
                        uint32_t body_last_rx_ms = body_start_ms;
                        while (copied < need) {
                            int64_t r = receive_from_socket(sock, body_copy + copied, need - copied, nullptr);
                            if (r == TCP_WOULDBLOCK) {
                                uint32_t now = (uint32_t)get_time();
                                if ((now - body_last_rx_ms) > policy.common.body_idle_timeout_ms || (now - body_start_ms) > policy.common.body_total_timeout_ms) break;
                                msleep(1);
                                continue;
                            }
                            if (r <= 0) break;
                            copied += (uint32_t)r;
                            body_last_rx_ms = (uint32_t)get_time();
                        }

                        if (copied < need) {
                            release(body_copy);
                            free_response(&resp);
                            string_free(buf);
                            if (sock) {
                                close_socket(sock);
                                sock = 0;
                            }
                            return error_response(SOCK_ERR_PROTO);
                        }

                        resp.body.ptr = (uintptr_t)body_copy;
                        resp.body.size = need;
                    }
                } else if (policy.allow_close_delimited) {
                    string body = string_repeat('\0', 0);
                    if (have) string_append_bytes(&body, buf.data + body_start, have);

                    uint32_t body_start_ms = (uint32_t)get_time();
                    uint32_t body_last_rx_ms = body_start_ms;
                    while (body.length <= policy.common.max_body_bytes) {
                        int64_t r = receive_from_socket(sock, tmp, sizeof(tmp), nullptr);
                        if (r == TCP_WOULDBLOCK) {
                            uint32_t now = (uint32_t)get_time();
                            if ((now - body_last_rx_ms) > policy.common.body_idle_timeout_ms || (now - body_start_ms) > policy.common.body_total_timeout_ms) {
                                string_free(body);
                                free_response(&resp);
                                string_free(buf);
                                if (sock) {
                                    close_socket(sock);
                                    sock = 0;
                                }
                                return error_response(SOCK_ERR_PROTO);
                            }
                            msleep(1);
                            continue;
                        }
                        if (r < 0) {
                            string_free(body);
                            free_response(&resp);
                            string_free(buf);
                            if (sock) {
                                close_socket(sock);
                                sock = 0;
                            }
                            return error_response(r);
                        }
                        if (r == 0) break;
                        if (body.length + (uint32_t)r > policy.common.max_body_bytes) {
                            string_free(body);
                            free_response(&resp);
                            string_free(buf);
                            if (sock) {
                                close_socket(sock);
                                sock = 0;
                            }
                            return error_response(SOCK_ERR_PROTO);
                        }
                        string_append_bytes(&body, tmp, (uint32_t)r);
                        body_last_rx_ms = (uint32_t)get_time();
                    }

                    if (body.length) {
                        resp.body.ptr = (uintptr_t)body.data;
                        resp.body.size = body.length;
                    } else if (body.mem_length) string_free(body);
                }
            }

            netlog_socket_event_t ev{};
            ev.comp = NETLOG_COMP_HTTP_CLIENT;
            ev.action = NETLOG_ACT_HTTP_RECV_RESPONSE;
            ev.pid = get_current_proc_pid();
            ev.u0 = (uint32_t)resp.status_code;
            ev.u1 = (uint32_t)resp.body.size;
            ev.local_port = get_socket_local_port(sock);
            net_l4_endpoint sock_remote_ep{};
            get_socket_remote_endpoint(sock, &sock_remote_ep);
            ev.remote_ep = sock_remote_ep;
            netlog_socket_event(&log_opts, &ev);

            string_free(buf);
            return resp;
        }
    }


public:
    explicit HTTPClient(const SocketExtraOptions* extra, const HTTPClientPolicyOptions* http_options) : sock(0), log_opts{}, tcp_extra(nullptr), policy(http_client_policy_from_options(http_options)), origin{} {
        if (extra) log_opts = *extra;

        if (extra && (log_opts.flags & SOCK_OPT_DEBUG)) {
            tcp_extra = (SocketExtraOptions*)zalloc(sizeof(SocketExtraOptions));
            if (tcp_extra) {
                *tcp_extra = *extra;
                tcp_extra->flags &= ~SOCK_OPT_DEBUG;
            }
        }

        sock = 0;
    }

    ~HTTPClient() {close();}

    int32_t set_options(const HTTPClientPolicyOptions* http_options) {
        policy = http_client_policy_from_options(http_options);
        return SOCK_OK;
    }

    int32_t connect_endpoint(const net_l4_endpoint* dst, const char* host_name = nullptr) {
        if (!sock) sock = create_socket(PROTO_TCP, tcp_extra ? tcp_extra : &log_opts);
        if (!sock) return SOCK_ERR_SYS;
        int32_t r = connect_socket(sock, dst);
        if (r >= 0) {
            HTTPClientOrigin next{};
            next.valid = true;
            if (dst) next.ep = *dst;
            if (host_name) next.domain = string_from_literal(host_name);
            if (origin.domain.mem_length) string_free(origin.domain);
            origin = next;
        }

        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_HTTP_CLIENT;
        ev.action = NETLOG_ACT_CONNECT;
        ev.pid = get_current_proc_pid();
        if (dst) ev.dst_ep = *dst;
        ev.i0 = r;

        if (sock) {
            ev.local_port = get_socket_local_port(sock);
            net_l4_endpoint sock_remote_ep{};
            get_socket_remote_endpoint(sock, &sock_remote_ep);
            ev.remote_ep = sock_remote_ep;
            if (ev.remote_ep.ver) ev.dst_ep = ev.remote_ep;
        }

        netlog_socket_event(&log_opts, &ev);
        return r;
    }

    int32_t connect_domain(const char* host, uint16_t port) {
        if (!host || !port) return SOCK_ERR_INVAL;
        net_l4_endpoint ep = socket_endpoint_select(host, port, (ip_version_t)0, DNS_USE_BOTH, 3000);
        if (!ep.ver) return SOCK_ERR_DNS;
        return connect_endpoint(&ep, host);
    }

    HTTPResponseMsg send_request(const HTTPRequestMsg& req) {
        HTTPRequestMsg curr = req;
        curr.path = string_repeat('\0', 0);
        if (req.path.data && req.path.length) string_append_bytes(&curr.path, req.path.data, req.path.length);
        curr.headers_common.fields.host = string_repeat('\0', 0);
        if (req.headers_common.fields.host.data && req.headers_common.fields.host.length) string_append_bytes(&curr.headers_common.fields.host, req.headers_common.fields.host.data, req.headers_common.fields.host.length);

        HTTPResponseMsg resp{};
        uint32_t redirects = policy.follow_redirects ? policy.max_redirects : 0;
        for (uint32_t i = 0;; i++) {
            if (!sock) {
                resp = error_response(SOCK_ERR_STATE);
                break;
            }

            if (!curr.host_override && curr.version != HTTP_VERSION_10 && !curr.headers_common.fields.host.length && origin.valid) {
                if (origin.domain.data) {
                    uint16_t host_port = origin.ep.port;
                    bool ipv6 = str_has_char(origin.domain.data, origin.domain.length, ':') >= 0;
                    if (host_port && host_port != 80) {
                        if (ipv6) curr.headers_common.fields.host = string_format("[%.*s]:%i", (int)origin.domain.length, origin.domain.data, (int)host_port);
                        else curr.headers_common.fields.host = string_format("%.*s:%i", (int)origin.domain.length, origin.domain.data, (int)host_port);
                    } else {
                        if (ipv6) curr.headers_common.fields.host = string_format("[%.*s]", (int)origin.domain.length, origin.domain.data);
                        else curr.headers_common.fields.host = string_from_literal_length(origin.domain.data, origin.domain.length);
                    }
                } else if (origin.ep.ver) {
                    char ip[48];
                    bool ipv6 = false;
                    uint16_t ep_port = 0;
                    net_ep_split(&origin.ep, ip, sizeof(ip), &ipv6, &ep_port);
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

            uint32_t off = 0;
            int64_t sent = 0;
            while (off < out_len) {
                int64_t r = send_on_socket(sock, (void*)(out.data + off), out_len - off);
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
            ev.pid = get_current_proc_pid();
            ev.u0 = out_len;
            ev.i0 = sent;
            ev.local_port = get_socket_local_port(sock);
            net_l4_endpoint sock_remote_ep{};
            get_socket_remote_endpoint(sock, &sock_remote_ep);
            ev.remote_ep = sock_remote_ep;

            char pathbuf[128];
            if (curr.path.length && curr.path.data) {
                uint32_t n = curr.path.length;
                if (n > sizeof(pathbuf) - 1) n = sizeof(pathbuf) - 1;
                memcpy(pathbuf, curr.path.data, n);
                pathbuf[n] = 0;
                ev.s0 = pathbuf;
            }

            netlog_socket_event(&log_opts, &ev);
            string_free(out);

            if (sent < 0) {
                resp = error_response(sent);
                break;
            }

            resp = receive_response(curr.method);
            uint32_t code = (uint32_t)resp.status_code;
            bool redirect = code == HTTP_MOVED_PERMANENTLY || code == HTTP_FOUND || code == HTTP_SEE_OTHER || code == HTTP_TEMPORARY_REDIRECT || code == HTTP_PERMANENT_REDIRECT;
            if (!redirect || i >= redirects || !resp.headers_common.fields.location.length) break;

            ParsedURL url = parse_url(resp.headers_common.fields.location.data, resp.headers_common.fields.location.length);
            if (!url.ok) break;

            bool absolute = url.scheme.size || url.host.size;
            uint16_t next_port = origin.ep.port;
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
                next_port = url.port ? url.port : 80;
                string next_domain = string_from_literal_length((const char*)url.host.ptr, url.host.size);
                if (!curr.host_override) {
                    if (curr.headers_common.fields.host.mem_length) string_free(curr.headers_common.fields.host);
                    curr.headers_common.fields.host = string_repeat('\0', 0);
                }

                if (sock) {
                        close_socket(sock);
                        sock = 0;
                    }
                sock = next_domain.data ? create_socket(PROTO_TCP, tcp_extra ? tcp_extra : &log_opts) : 0;
                int32_t rr = sock ? SOCK_OK : SOCK_ERR_SYS;
                if (rr >= 0) rr = connect_domain(next_domain.data, next_port);
                if (next_domain.mem_length) string_free(next_domain);
                if (rr < 0) {
                    free_response(&resp);
                    resp.status_code = (HttpError)rr;
                    break;
                }
            } else {
                net_l4_endpoint reconnect_ep = origin.ep;
                string reconnect_domain = string{};
                if (origin.domain.data) reconnect_domain = string_from_literal(origin.domain.data);

                if (sock) {
                        close_socket(sock);
                        sock = 0;
                    }
                sock = origin.valid ? create_socket(PROTO_TCP, tcp_extra ? tcp_extra : &log_opts) : 0;
                int32_t rr = sock ? SOCK_OK : SOCK_ERR_SYS;
                if (rr >= 0) rr = reconnect_domain.data ? connect_domain(reconnect_domain.data, reconnect_ep.port) : connect_endpoint(&reconnect_ep);
                if (reconnect_domain.mem_length) string_free(reconnect_domain);
                if (rr < 0) {
                    free_response(&resp);
                    resp.status_code = (HttpError)rr;
                    break;
                }
            }

            if (code == HTTP_SEE_OTHER && curr.method != HTTP_METHOD_HEAD) {
                curr.method = HTTP_METHOD_GET;
                curr.body = sizedptr{};
                curr.headers_common.framing.chunked = 0;
                curr.headers_common.framing.has_content_length = 0;
                curr.headers_common.fields.content_length = 0;
            }

            free_response(&resp);
        }

        if (curr.path.mem_length) string_free(curr.path);
        if (curr.headers_common.fields.host.mem_length) string_free(curr.headers_common.fields.host);
        return resp;
    }

    int32_t close() {
        netlog_socket_event_t ev{};
        ev.comp = NETLOG_COMP_HTTP_CLIENT;
        ev.action = NETLOG_ACT_CLOSE;
        ev.pid = get_current_proc_pid();

        if (sock) {
            ev.local_port = get_socket_local_port(sock);
            net_l4_endpoint sock_remote_ep{};
            get_socket_remote_endpoint(sock, &sock_remote_ep);
            ev.remote_ep = sock_remote_ep;
        }

        int32_t r = SOCK_ERR_STATE;
        if (sock) {
            r = close_socket(sock);
            sock = 0;
        }
        ev.i0 = r;
        netlog_socket_event(&log_opts, &ev);

        if (tcp_extra) release(tcp_extra);
        tcp_extra = nullptr;

        if (origin.domain.mem_length) string_free(origin.domain);
        origin = HTTPClientOrigin{};
        log_opts.flags &= ~SOCK_OPT_DEBUG;
        return r;
    }
};
