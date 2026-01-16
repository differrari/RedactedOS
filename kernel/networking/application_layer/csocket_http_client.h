#pragma once
#include "http.h"
#include "net/socket_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* http_client_handle_t;

http_client_handle_t http_client_create(uint16_t pid, const SocketExtraOptions* extra);
void http_client_destroy(http_client_handle_t h);

int32_t http_client_connect_ex(http_client_handle_t h, uint8_t dst_kind, const void *dst, uint16_t port);

HTTPResponseMsg http_client_send_request(http_client_handle_t h, const HTTPRequestMsg *req);

int32_t http_client_close(http_client_handle_t h);

#ifdef __cplusplus
}
#endif
