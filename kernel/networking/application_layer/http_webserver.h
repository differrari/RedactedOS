#pragma once

#include "http.h"
#include "csocket_http_server.h"
#include "net/socket_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HTTPWebContext HTTPWebContext;
typedef int32_t (*HTTPWebHandler)(HTTPWebContext *ctx);

typedef enum {
    HTTP_ROUTE_EXACT = 0,
    HTTP_ROUTE_PREFIX = 1u << 0,
    HTTP_ROUTE_HEAD_AS_GET = 1u << 1
} HTTPRouteFlags;

typedef enum {
    HTTP_ROUTE_HANDLER,
    HTTP_ROUTE_STATIC,
    HTTP_ROUTE_FILE
} HTTPRouteKind;

typedef struct {
    HttpError status;
    const char *content_type;
    const void *body;
    uint32_t body_len;
    const HTTPHeader *headers;
    uint32_t header_count;
} HTTPWebResponse;

typedef struct {
    const char *fs_path;
    const char *content_type;
    uint32_t max_bytes;
    uint32_t cache_max_age_sec;
    const HTTPHeader *headers;
    uint32_t header_count;
} HTTPWebFile;

typedef struct {
    const char *path;
    uint32_t methods;
    uint32_t flags;
    HTTPRouteKind kind;
    void *user;
    union {
        HTTPWebHandler handler;
        HTTPWebResponse response;
        HTTPWebFile file;
    } as;
} HTTPRoute;

typedef struct {
    uint16_t port;
    int backlog;
    const SocketOptions *socket_options;
    const HTTPServerPolicyOptions *policy_options;
    const HTTPRoute *routes;
    uint32_t route_count;
    HTTPWebResponse not_found;
    void *user;
    const char *mdns_instance;
    const char *mdns_type;
    const char *mdns_proto;
    const char *mdns_txt;
    bool close_each_response;
    bool head_as_get;
    bool options_for_any_path;
} HTTPWebServerConfig;

struct HTTPWebContext {
    http_server_handle_t server;
    http_connection_handle_t conn;
    HTTPRequestMsg *request;
    const HTTPRoute *route;
    const HTTPWebServerConfig *config;
    void *user;
    bool close_after_response;
};

#define HTTP_WEB_RESPONSE(code_, type_, body_, body_len_) (HTTPWebResponse){code_, type_, body_, body_len_, NULL, 0}
#define HTTP_WEB_TEXT_RESPONSE(code_, text_) (HTTPWebResponse){code_, "text/plain", text_, sizeof(text_) - 1, NULL, 0}
#define HTTP_WEB_HTML_RESPONSE(code_, html_) (HTTPWebResponse){code_, "text/html", html_, sizeof(html_) - 1, NULL, 0}

int32_t http_webserver_run(const HTTPWebServerConfig *config);
int32_t http_web_send(HTTPWebContext *ctx, const HTTPWebResponse *response);

#ifdef __cplusplus
}
#endif
