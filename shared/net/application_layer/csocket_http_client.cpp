#pragma once
#include "csocket_http_client.h"
#include "socket_http_client.hpp" 
#include "net/transport_layer/socket_tcp.hpp"

extern "C" {
    extern uintptr_t malloc(uint64_t size);
    extern void      free(void *ptr, uint64_t size);
    extern void      sleep(uint64_t ms);
}

extern "C" {

http_client_handle_t http_client_create(uint16_t pid) {
    uintptr_t mem = malloc(sizeof(HTTPClient));
    if (!mem) return NULL;
    HTTPClient *cli = reinterpret_cast<HTTPClient*>( (void*)mem );
    return reinterpret_cast<http_client_handle_t>(new HTTPClient(pid));
}

void http_client_destroy(http_client_handle_t h) {
    if (!h) return;
    HTTPClient *cli = reinterpret_cast<HTTPClient*>(h);
    cli->~HTTPClient();
    free(cli, sizeof(HTTPClient));
}

int32_t http_client_connect(http_client_handle_t h,
                            uint32_t ip,
                            uint16_t port)
{
    if (!h) return (int32_t)SOCK_ERR_INVAL;
    HTTPClient *cli = reinterpret_cast<HTTPClient*>(h);
    return cli->connect(ip, port);
}

HTTPResponseMsg http_client_send_request(http_client_handle_t h,
                                        const HTTPRequestMsg *req)
{
    HTTPResponseMsg empty;
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
