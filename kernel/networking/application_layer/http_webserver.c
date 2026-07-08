#include "http_webserver.h"
#include "filesystem/filesystem.h"
#include "networking/application_layer/dns/mdns_responder.h"
#include "networking/transport_layer/socket_bind.h"
#include "std/memory.h"
#include "std/string.h"
#include "syscalls/syscalls.h"

typedef struct {
    const HTTPRoute *route;
    const HTTPRoute *method_route;
    uint32_t methods;
} HTTPWebRouteMatch;

static uint32_t http_web_path_len(string path) {
    uint32_t len = path.length;
    int32_t q = str_has_char(path.data, len, '?');
    if (q >= 0)len = (uint32_t)q;
    int32_t f = str_has_char(path.data, len, '#');
    if (f >= 0)len = (uint32_t)f;
    return len;
}

static HTTPWebRouteMatch http_web_find_route(const HTTPWebServerConfig *config, string path, HTTPMethod method) {
    HTTPWebRouteMatch match = {0};
    if (!config || (!config->routes && config->route_count)) return match;

    uint32_t path_len = http_web_path_len(path);
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

static int32_t http_web_send_blob(HTTPWebContext *ctx, const HTTPWebFile *file, const void *body, uint32_t body_len) {
    static char cache_name[] = "Cache-Control";
    string cache = {0};
    HTTPHeader local[4];
    uint32_t base_count = file && file->headers ? file->header_count : 0;
    uint32_t total = base_count;

    if (file && file->cache_max_age_sec) total++;
    HTTPHeader *headers = total ? local : NULL;
    if (total > N_ARR(local)) headers = (HTTPHeader*)zalloc(sizeof(HTTPHeader) * total);
    if (total && !headers) {
        HTTPWebResponse response = HTTP_WEB_TEXT_RESPONSE(HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error\n");
        return http_web_send(ctx, &response);
    }

    for (uint32_t i = 0; i < base_count; i++) headers[i] = file->headers[i];
    if (file && file->cache_max_age_sec) {
        cache = string_format("public, max-age=%i", (int)file->cache_max_age_sec);
        headers[base_count] = (HTTPHeader){{cache_name, sizeof(cache_name) - 1, 0}, cache};
    }

    HTTPWebResponse response = {
        .status = HTTP_OK,
        .content_type = file && file->content_type ? file->content_type : "application/octet-stream",
        .body = body,
        .body_len = body_len,
        .headers = headers,
        .header_count = total
    };
    
    int32_t rc = http_web_send(ctx, &response);
    if (headers && headers != local) release(headers);
    string_free(cache);
    return rc;
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

static int32_t http_web_send_file(HTTPWebContext *ctx, const HTTPWebFile *file) {
    if (!ctx || !file || !file->fs_path) return SOCK_ERR_INVAL;

    fs_stat st = {0};
    if (!get_stat(kernel_fs(), file->fs_path, &st) || st.type != entry_file) {
        HTTPWebResponse response = HTTP_WEB_TEXT_RESPONSE(HTTP_NOT_FOUND, "Not Found\n");
        return http_web_send(ctx, &response);
    }
    if (st.size > UINT32_MAX || (file->max_bytes && st.size > file->max_bytes)) {
        HTTPWebResponse response = HTTP_WEB_TEXT_RESPONSE(HTTP_PAYLOAD_TOO_LARGE, "Payload Too Large\n");
        return http_web_send(ctx, &response);
    }

    uint32_t size = (uint32_t)st.size;
    uint8_t *buf = NULL;
    bool head = ctx->request && ctx->request->method == HTTP_METHOD_HEAD;
    if (size && !head) {
        buf = (uint8_t*)zalloc(size);
        if (!buf) {
            HTTPWebResponse response = HTTP_WEB_TEXT_RESPONSE(HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error\n");
            return http_web_send(ctx, &response);
        }
        if (simple_read(kernel_fs(), file->fs_path, buf, size) != size) {
            release(buf);
            HTTPWebResponse response = HTTP_WEB_TEXT_RESPONSE(HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error\n");
            return http_web_send(ctx, &response);
        }
    }

    int32_t rc = http_web_send_blob(ctx, file, buf, size);
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

    if (config->mdns_instance && config->mdns_type && config->mdns_proto) mdns_register_service(config->mdns_instance, config->mdns_type, config->mdns_proto, config->port, config->mdns_txt);

    while (1) {
        http_connection_handle_t conn = http_server_accept(srv);
        if (!conn) {
            msleep(50);
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
