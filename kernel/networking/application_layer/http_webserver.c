#include "http_webserver.h"
#include "networking/firewall.h"
#include "filesystem/filesystem.h"
#include "networking/application_layer/dns/mdns_responder.h"
#include "networking/transport_layer/socket_bind.h"
#include "data/format/url.h"
#include "std/memory.h"
#include "std/string.h"
#include "syscalls/syscalls.h"
#include "process/scheduler.h"

typedef struct {
    const HTTPRoute *route;
    const HTTPRoute *method_route;
    uint32_t methods;
} HTTPWebRouteMatch;

static HTTPWebRouteMatch http_web_find_route(const HTTPWebServerConfig *config, string path, HTTPMethod method) {
    HTTPWebRouteMatch match = {0};
    if (!config || (!config->routes && config->route_count)) return match;

    uint32_t path_len = url_path_len(path);
    uint32_t best_prefix_len = 0;
    uint32_t best_method_prefix_len = 0;
    uint32_t exact_methods = 0;
    uint32_t prefix_methods = 0;
    bool exact = false;

    for (uint32_t i = 0; i < config->route_count; i++) {
        const HTTPRoute *route = &config->routes[i];
        if (!route->path || !path.data) continue;

        uint32_t route_len = (uint32_t)strlen(route->path);
        bool prefix = (route->flags & HTTP_ROUTE_PREFIX) != 0;
        if (!(prefix ? path_len >= route_len && memcmp(path.data, route->path, route_len) == 0 : path_len == route_len && memcmp(path.data, route->path, route_len) == 0)) continue;

        uint32_t methods = route->methods;
        if ((methods & HTTP_METHOD_MASK_GET) && (config->head_as_get || (route->flags & HTTP_ROUTE_HEAD_AS_GET))) methods |= HTTP_METHOD_MASK_HEAD;
        bool allowed = http_method_allowed(methods, method);

        if (!prefix) {
            exact = true;
            exact_methods |= methods;
            if (!match.route) match.route = route;
            if (allowed && !match.method_route) match.method_route = route;
        } else if (!exact) {
            if (route_len >= best_prefix_len) {
                if (route_len > best_prefix_len) {
                    match.route = route;
                    prefix_methods = 0;
                    best_prefix_len = route_len;
                }
                prefix_methods |= methods;
            }
            if (allowed && route_len >= best_method_prefix_len) {
                match.method_route = route;
                best_method_prefix_len = route_len;
            }
        }
    }

    match.methods = exact_methods ? exact_methods : prefix_methods;
    return match;
}

static HTTPHeader *http_web_build_file_headers(const HTTPWebFile *file, string content_range, HTTPHeader local[8], uint32_t *out_count, string *out_cache) {
    static char cache_name[] = "Cache-Control";
    static char accept_ranges_name[] = "Accept-Ranges";
    static char accept_ranges_value[] = "bytes";
    static char content_range_name[] = "Content-Range";

    *out_count = 0;
    *out_cache = (string){0};
    uint32_t base_count = file->headers ? file->header_count : 0;
    uint32_t total = base_count+1;

    if (file->cache_max_age_sec) total++;
    if (content_range.length) total++;
    HTTPHeader *headers = total <= 8 ? local : (HTTPHeader*)zalloc(sizeof(HTTPHeader) * total);
    if (!headers) return NULL;
    uint32_t n = 0;
    for (uint32_t i = 0; i < base_count; i++) headers[n++] = file->headers[i];
    headers[n++] = (HTTPHeader){{accept_ranges_name, sizeof(accept_ranges_name) - 1, 0}, {accept_ranges_value, sizeof(accept_ranges_value) - 1, 0}};
    if (file->cache_max_age_sec) {
        *out_cache = string_format("public, max-age=%i", (int)file->cache_max_age_sec);
        headers[n++] = (HTTPHeader){{cache_name, sizeof(cache_name) - 1, 0}, *out_cache};
    }

    if (content_range.length) headers[n++] = (HTTPHeader){{content_range_name, sizeof(content_range_name) - 1, 0}, content_range};

    *out_count = n;
    return headers;
}

int32_t http_web_send(HTTPWebContext *ctx, const HTTPWebResponse *response) {
    if (!ctx || !response) return SOCK_ERR_INVAL;
    static char close_data[] = "close";
    const char *type = response->content_type ? response->content_type : "application/octet-stream";
    HTTPResponseMsg res = {0};
    res.status_code = response->status;
    res.headers_common.fields.content_length = response->body_len;
    res.headers_common.framing.has_content_length = 1;
    res.headers_common.fields.content_type = (string){(char*)type, (uint32_t)strlen(type), 0};
    if (ctx->close_after_response) res.headers_common.fields.connection = (string){close_data, sizeof(close_data) - 1, 0};
    if (response->body && response->body_len) res.body = (string){(char*)response->body, response->body_len, 0};
    res.extra_headers = (HTTPHeader*)response->headers;
    res.extra_header_count = response->header_count;
    return http_server_send_response(ctx->server, ctx->conn, &res);
}

static int32_t http_web_send_allow(HTTPWebContext *ctx, HttpError status, uint32_t methods) {
    methods |= HTTP_METHOD_MASK_OPTIONS;
    string allow = http_methods_allow_header(methods);
    string body = status == HTTP_METHOD_NOT_ALLOWED ? string_format("%s\n", http_status_reason(status)) : (string){0};
    static char allow_name[] = "Allow";
    static char text_plain[] = "text/plain";
    HTTPHeader allow_header = {{allow_name, sizeof(allow_name)-1, 0}, allow};
    HTTPResponseMsg res = {0};
    res.status_code = status;
    res.headers_common.fields.content_length = body.length;
    res.headers_common.framing.has_content_length = 1;
    res.headers_common.fields.content_type = (string){text_plain, sizeof(text_plain) - 1, 0};
    res.extra_headers = &allow_header;
    res.extra_header_count = 1;
    res.body = body;
    int32_t rc = http_server_send_response(ctx->server, ctx->conn, &res);
    string_free(allow);
    string_free(body);
    return rc;
}

static int32_t http_web_send_file(HTTPWebContext *ctx, const HTTPWebFile *web_file) {
    if (!ctx || !web_file || !web_file->fs_path) return SOCK_ERR_INVAL;

    fs_stat st = {0};
    if (!get_stat(kernel_fs(), web_file->fs_path, &st) || st.type != entry_file) {
        HTTPWebResponse response = HTTP_WEB_TEXT_RESPONSE(HTTP_NOT_FOUND, "Not Found\n");
        return http_web_send(ctx, &response);
    }
    if (st.size > UINT32_MAX || (web_file->max_bytes && st.size > web_file->max_bytes)) {
        HTTPWebResponse response = HTTP_WEB_TEXT_RESPONSE(HTTP_PAYLOAD_TOO_LARGE, "Payload Too Large\n");
        return http_web_send(ctx, &response);
    }

    uint64_t file_size = st.size;
    uint64_t range_start = 0;
    uint64_t range_end = file_size ? file_size - 1 : 0;
    uint64_t range_len = file_size;
    bool partial = ctx->request->headers_common.range.has;
    bool not_satisfiable = false;

    if (partial) {
        const HTTPRangeSpec *in = &ctx->request->headers_common.range;

        if (in->invalid || file_size == 0 || (!in->has_start && !in->has_end)) not_satisfiable = true;
        else if (in->has_start) {
            range_start = in->start;
            range_end = in->has_end && in->end < file_size ? in->end : file_size - 1;
            not_satisfiable = range_start >= file_size || range_end < range_start;
        } else {
            uint64_t suffix = in->end;
            not_satisfiable = suffix == 0;
            if (!not_satisfiable) {
                range_start = suffix >= file_size ? 0 : file_size - suffix;
                range_end = file_size - 1;
            }
        }

        range_len = not_satisfiable ? 0 : range_end - range_start + 1;
    }

    char content_range_buf[64];
    string content_range = {0};
    if (not_satisfiable) {
        //print("aaa %s %llu", web_file->fs_path, file_size);
        content_range.length = (uint32_t)string_format_buf(content_range_buf, sizeof(content_range_buf), "bytes */%llu", file_size);
        content_range.data = content_range_buf;

        HTTPHeader local[8];
        uint32_t total = 0;
        string cache = {0};
        HTTPHeader *headers = http_web_build_file_headers(web_file, content_range, local, &total, &cache);
        if (!headers) {
            HTTPWebResponse response = HTTP_WEB_TEXT_RESPONSE(HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error\n");
            return http_web_send(ctx, &response);
        }

        HTTPResponseMsg res = {0};
        res.status_code = HTTP_RANGE_NOT_SATISFIABLE;
        res.headers_common.fields.content_length = 0;
        res.headers_common.framing.has_content_length = 1;
        res.extra_headers = headers;
        res.extra_header_count = total;

        int32_t rc = http_server_send_response(ctx->server, ctx->conn, &res);
        if (headers != local) release(headers);
        string_free(cache);
        return rc;
    }

    if (partial) {
        //print("%s %llu-%llu %llu", web_file->fs_path, range_start, range_end, file_size);
        content_range.length = (uint32_t)string_format_buf(content_range_buf, sizeof(content_range_buf), "bytes %llu-%llu/%llu", range_start, range_end, file_size);
        content_range.data = content_range_buf;
    }

    uint32_t read_len = (uint32_t)range_len;
    uint8_t *buf = NULL;
    bool head = ctx->request->method == HTTP_METHOD_HEAD;
    if (read_len && !head) {
        buf = (uint8_t*)zalloc(read_len);
        if (!buf) {
            HTTPWebResponse response = HTTP_WEB_TEXT_RESPONSE(HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error\n");
            return http_web_send(ctx, &response);
        }
        file fd = {0};
        FS_RESULT ores = open_file(kernel_fs(), web_file->fs_path, &fd);
        if (ores != FS_RESULT_SUCCESS) {
            release(buf);
            HTTPWebResponse response = HTTP_WEB_TEXT_RESPONSE(HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error\n");
            return http_web_send(ctx, &response);
        }
        fd.cursor = range_start;
        size_t got = read_file(&fd, (char*)buf, read_len);
        close_file(&fd);
        if (got != read_len) {
            release(buf);
            HTTPWebResponse response = HTTP_WEB_TEXT_RESPONSE(HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error\n");
            return http_web_send(ctx, &response);
        }
    }

    HTTPHeader local[8];
    uint32_t total = 0;
    string cache = {0};
    HTTPHeader *headers = http_web_build_file_headers(web_file, content_range, local, &total, &cache);
    if (!headers) {
        if (buf) release(buf);
        HTTPWebResponse response = HTTP_WEB_TEXT_RESPONSE(HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error\n");
        return http_web_send(ctx, &response);
    }

    HTTPWebResponse response = {
        .status = partial ? HTTP_PARTIAL_CONTENT : HTTP_OK,
        .content_type = web_file->content_type ? web_file->content_type : "application/octet-stream",
        .body = buf,
        .body_len = read_len,
        .headers = headers,
        .header_count = total
    };

    int32_t rc = http_web_send(ctx, &response);
    if (headers != local) release(headers);
    string_free(cache);
    if (buf) release(buf);
    return rc;
}

int32_t http_webserver_run(const HTTPWebServerConfig *config) {
    if (!config || !config->port || (!config->routes && config->route_count)) return SOCK_ERR_INVAL;

    HTTPServerPolicyOptions local_policy = config->policy_options ? *config->policy_options : (HTTPServerPolicyOptions){0};
    if (!(local_policy.flags & HTTP_SERVER_OPT_ALLOWED_METHODS)) {
        uint32_t methods = HTTP_METHOD_MASK_OPTIONS;
        for (uint32_t i = 0; i < config->route_count; i++) {
            methods |= config->routes[i].methods;
            if ((config->routes[i].methods & HTTP_METHOD_MASK_GET) && (config->head_as_get || (config->routes[i].flags & HTTP_ROUTE_HEAD_AS_GET))) methods |= HTTP_METHOD_MASK_HEAD;
        }
        local_policy.flags |= HTTP_SERVER_OPT_ALLOWED_METHODS;
        local_policy.value.allowed_methods = methods;
    }

    http_server_handle_t srv = http_server_create(config->socket_options, &local_policy);
    if (!srv) return SOCK_ERR_SYS;

    struct SockBindSpec spec = {0};
    spec.kind = BIND_ANY;
    int32_t rc = http_server_bind(srv, &spec, config->port);
    if (rc < 0) {
        http_server_destroy(srv);
        return rc;
    }

    rc = http_server_listen(srv, config->backlog > 0 ? config->backlog : 8);
    if (rc < 0) {
        http_server_close(srv);
        http_server_destroy(srv);
        return rc;
    }

    if (config->firewall_allow) {
        NetCtrlFirewallRule rule = {
            .action = NET_CTRL_FIREWALL_ALLOW,
            .direction = NET_CTRL_FIREWALL_IN,
            .protocol = PROTO_TCP,
            .port_from = config->port,
            .port_to = config->port
        };
        uint16_t pid = get_current_proc_pid();
        rc = pid ? firewall_add_rule(&rule, pid) : SOCK_ERR_INVAL;
        if (rc < 0) {
            http_server_close(srv);
            http_server_destroy(srv);
            return rc;
        }
    }

    if (config->mdns_instance && config->mdns_type && config->mdns_proto) mdns_register_service(config->mdns_instance, config->mdns_type, config->mdns_proto, config->port, config->mdns_txt);

    while (1) {
        http_connection_handle_t conn = http_server_accept(srv);
        if (!conn) {
            msleep(10);
            continue;
        }

        while (2) {
            HTTPRequestMsg req = http_server_recv_request(srv, conn);
            if (!req.path.length) {
                http_connection_close(conn);
                break;
            }

            HTTPWebRouteMatch match = http_web_find_route(config, req.path, req.method);
            const HTTPRoute *route = match.method_route ? match.method_route : match.route;
            HTTPWebContext ctx = {
                .server = srv,
                .conn = conn,
                .request = &req,
                .route = route,
                .config = config,
                .user = route && route->user ? route->user : config->user,
                .close_after_response = config->close_each_response
            };

            if (req.method == HTTP_METHOD_OPTIONS && (match.route || config->options_for_any_path)) rc = http_web_send_allow(&ctx, HTTP_OK, match.methods);
            else if (match.method_route) {
                if (match.method_route->kind == HTTP_ROUTE_STATIC) rc = http_web_send(&ctx, &match.method_route->as.response);
                else if (match.method_route->kind == HTTP_ROUTE_FILE) rc = http_web_send_file(&ctx, &match.method_route->as.file);
                else if (match.method_route->kind == HTTP_ROUTE_HANDLER && match.method_route->as.handler) rc = match.method_route->as.handler(&ctx);
                else rc = SOCK_ERR_INVAL;
            } else if (match.route) rc = http_web_send_allow(&ctx, HTTP_METHOD_NOT_ALLOWED, match.methods);
            else if (config->not_found.body || config->not_found.body_len || config->not_found.content_type) rc = http_web_send(&ctx, &config->not_found);
            else {
                HTTPWebResponse response = HTTP_WEB_TEXT_RESPONSE(HTTP_NOT_FOUND, "Not Found\n");
                rc = http_web_send(&ctx, &response);
            }

            http_request_free(&req);
            if (config->close_each_response || rc < 0) {
                http_connection_close(conn);
                break;
            }
        }
    }
}
