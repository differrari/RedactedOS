#pragma once
#include "types.h"
#include "http.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* http_server_handle_t;
typedef void* http_connection_handle_t;

http_server_handle_t http_server_create(uint16_t pid);

void http_server_destroy(http_server_handle_t srv);

int32_t http_server_bind(http_server_handle_t srv,
                        uint16_t port);

int32_t http_server_listen(http_server_handle_t srv,
                           int backlog);

http_connection_handle_t http_server_accept(http_server_handle_t srv);

HTTPRequestMsg http_server_recv_request(http_server_handle_t srv,
                                        http_connection_handle_t conn);

int32_t http_server_send_response(http_server_handle_t srv,
                                http_connection_handle_t conn,
                                const HTTPResponseMsg* res);

int32_t http_connection_close(http_connection_handle_t conn);

int32_t http_server_close(http_server_handle_t srv);

#ifdef __cplusplus
}
#endif
