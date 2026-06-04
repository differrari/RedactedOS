#pragma once
#include "http.h"
#include "net/socket_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* http_client_handle_t;

http_client_handle_t http_client_create(const SocketExtraOptions* extra, const HTTPClientPolicyOptions *options);
int32_t http_client_set_options(http_client_handle_t h, const HTTPClientPolicyOptions *options);
void http_client_destroy(http_client_handle_t h);

int32_t http_client_connect_endpoint(http_client_handle_t h, const net_l4_endpoint *dst);
int32_t http_client_connect_domain(http_client_handle_t h, const char *host, uint16_t port);

HTTPResponseMsg http_client_send_request(http_client_handle_t h, const HTTPRequestMsg *req);

int32_t http_client_close(http_client_handle_t h);

#ifdef __cplusplus
}
#endif
