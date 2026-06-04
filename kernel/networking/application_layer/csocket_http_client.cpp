#include "csocket_http_client.h"
#include "socket_http_client.hpp"

extern "C" {

http_client_handle_t http_client_create(const SocketExtraOptions* extra, const HTTPClientPolicyOptions *options) {
    HTTPClient* cli = new HTTPClient(extra, options);
    if (!cli) return nullptr;
    return reinterpret_cast<http_client_handle_t>(cli);
}

void http_client_destroy(http_client_handle_t h) {
    if (!h) return;
    HTTPClient* cli = reinterpret_cast<HTTPClient*>(h);
    delete cli;
}

int32_t http_client_set_options(http_client_handle_t h, const HTTPClientPolicyOptions *options) {
    if (!h) return (int32_t)SOCK_ERR_INVAL;
    HTTPClient* cli = reinterpret_cast<HTTPClient*>(h);
    return cli->set_options(options);
}

int32_t http_client_connect_endpoint(http_client_handle_t h, const net_l4_endpoint* dst) {
    if (!h || !dst) return (int32_t)SOCK_ERR_INVAL;
    HTTPClient *cli = reinterpret_cast<HTTPClient*>(h);
    return cli->connect_endpoint(dst);
}

int32_t http_client_connect_domain(http_client_handle_t h, const char* host, uint16_t port) {
    if (!h || !host) return (int32_t)SOCK_ERR_INVAL;
    HTTPClient *cli = reinterpret_cast<HTTPClient*>(h);
    return cli->connect_domain(host, port);
}

HTTPResponseMsg http_client_send_request(http_client_handle_t h, const HTTPRequestMsg *req) {
    HTTPResponseMsg empty{};
    if (!h || !req) {
        empty.status_code = (HttpError)SOCK_ERR_INVAL;
        return empty;
    }
    HTTPClient *cli = reinterpret_cast<HTTPClient*>(h);
    return cli->send_request(*req);
}

int32_t http_client_close(http_client_handle_t h) {
    if (!h) return (int32_t)SOCK_ERR_INVAL;
    HTTPClient *cli = reinterpret_cast<HTTPClient*>(h);
    return cli->close();
}
}
