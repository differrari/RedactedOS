#include "curl.h"
#include "console/kio.h"
#include "data/format/url.h"
#include "networking/application_layer/csocket_http_client.h"
#include "networking/application_layer/http.h"
#include "process/scheduler.h"
#include "std/memory.h"
#include "std/string.h"
#include "syscalls/syscalls.h"

#define CURL_MAX_REDIRECTS 5

typedef struct {
    char *url;
    bool head_only;
    bool follow;
} curl_opts_t;

static int curl_fetch(char *url, bool head_only, bool follow) {
    string host = (string){0};
    string path = (string){0};
    uint16_t port = 0;
    bool https = false;

    if (!url) {
        print("curl: bad url");
        return 2;
    }

    ParsedURL parsed = parse_url(url, (uint32_t)strlen(url));
    if (!parsed.ok || !parsed.scheme.ptr || !parsed.host.ptr || !parsed.host.size) {
        print("curl: bad url");
        return 2;
    }

    if (parsed.scheme.size == 5 && strncmp_case((const char*)parsed.scheme.ptr, "https", true, 5) == 0) {
        https = true;
        port = parsed.port ? parsed.port : 443;
    } else if (parsed.scheme.size == 4 && strncmp_case((const char*)parsed.scheme.ptr, "http", true, 4) == 0) port = parsed.port ? parsed.port : 80;
    else {
        print("curl: bad url");
        return 2;
    }

    host = string_from_literal_length((const char*)parsed.host.ptr, parsed.host.size);
    if (!host.data) {
        print("curl: bad url");
        return 2;
    }

    path = string_repeat('\0', 0);
    if (parsed.path.ptr && parsed.path.size) string_append_bytes(&path, (const char*)parsed.path.ptr, parsed.path.size);
    else string_append_bytes(&path, "/", 1);
    if (parsed.query.ptr && parsed.query.size) {
        string_append_bytes(&path, "?", 1);
        string_append_bytes(&path, (const char*)parsed.query.ptr, parsed.query.size);
    }

    if (!path.data) {
        string_free(host);
        print("curl: bad url");
        return 2;
    }

    if (https) {
        string_free(host);
        string_free(path);
        print("curl: https is not supported yet");
        return 3;
    }

    HTTPClientPolicyOptions opts = {0};
    opts.flags = HTTP_CLIENT_OPT_FOLLOW_REDIRECTS | HTTP_CLIENT_OPT_MAX_REDIRECTS;
    opts.value.follow_redirects = follow != 0;
    opts.value.max_redirects = CURL_MAX_REDIRECTS;

    http_client_handle_t cli = http_client_create(NULL, &opts);
    if (!cli) {
        string_free(host);
        string_free(path);
        print("curl: socket create failed");
        return 4;
    }

    int32_t rc = http_client_connect_domain(cli, host.data, port);
    if (rc < 0) {
        print("curl: connect failed for %s:%d", host.data, port);
        http_client_destroy(cli);
        string_free(host);
        string_free(path);
        return 5;
    }

    HTTPRequestMsg req = (HTTPRequestMsg){0};
    req.method = head_only ? HTTP_METHOD_HEAD : HTTP_METHOD_GET;
    req.version = HTTP_VERSION_11;
    req.path = path;
    req.headers_common.fields.connection = string_from_const("close");

    HTTPResponseMsg resp = http_client_send_request(cli, &req);
    if ((int32_t)resp.status_code < 0) {
        print("curl: request failed (%d)", (int)resp.status_code);
        http_response_free(&resp);
        http_headers_common_free(&req.headers_common);
        http_client_destroy(cli);
        string_free(host);
        string_free(path);
        return 6;
    }

    if (head_only) {
        HTTPResponseMsg head = resp;
        head.body = (string){0};
        string raw = http_response_builder(&head);
        print("%.*s", (int)raw.length, raw.data);
        string_free(raw);
    } else if (resp.body.data && resp.body.length) print("%.*s", (int)resp.body.length, (const char*)resp.body.data);

    http_response_free(&resp);
    http_headers_common_free(&req.headers_common);

    http_client_destroy(cli);
    string_free(host);
    string_free(path);
    return 0;
}

static bool parse_args(int argc, char *argv[], curl_opts_t *o) {
    memset(o, 0, sizeof(*o));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-I") == 0) o->head_only = true;
        else if (strcmp(argv[i], "-L") == 0) o->follow = true;
        else if (!o->url) o->url = argv[i];
        else return false;
    }

    return o->url != NULL;
}

int run_curl(int argc, char* argv[]) {
    curl_opts_t opts;
    if (!parse_args(argc, argv, &opts)) {
        print("usage: curl [-L] [-I] http://host/path");
        return 2;
    }

    return curl_fetch(opts.url, opts.head_only, opts.follow);
}
