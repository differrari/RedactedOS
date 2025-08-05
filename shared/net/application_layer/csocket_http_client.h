#pragma once

#include "http.h"
#include "std/string.h"
#include "std/memfunctions.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* http_client_handle_t;

http_client_handle_t http_client_create(uint16_t pid);
void http_client_destroy(http_client_handle_t h);

int32_t http_client_connect(http_client_handle_t h,
                            uint32_t ip,
                            uint16_t port);

HTTPResponseMsg http_client_send_request(http_client_handle_t h,
                                        const HTTPRequestMsg *req);

int32_t http_client_close(http_client_handle_t h);

#ifdef __cplusplus
}
#endif
