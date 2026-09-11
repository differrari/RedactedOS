#pragma once

#include "networking/transport_layer/csocket.h"
#include "networking/net_logger/net_logger.h"
#include "std/string.h"

#define HTTP_SOCKET_WRITE_IDLE_TIMEOUT_MS 3000
#define HTTP_SOCKET_WRITE_TOTAL_TIMEOUT_MS 30000

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HTTPSocketTimeoutState {
    uint32_t start_ms;
    uint32_t last_progress_ms;
} HTTPSocketTimeoutState;

int32_t http_socket_close(socket_handle_t *socket, bool abort);
int64_t http_socket_recv_wait(socket_handle_t socket,
                            void *buffer,
                            uint32_t length, 
                            uint32_t idle_timeout_ms,
                            uint32_t total_timeout_ms,
                            HTTPSocketTimeoutState *timeout);
int32_t http_socket_recv_head(socket_handle_t socket,
                            string *buffer,
                            uint32_t max_header_bytes,
                            uint32_t idle_timeout_ms,
                            uint32_t total_timeout_ms,
                            int32_t *header_end);
int64_t http_socket_recv_exact(socket_handle_t socket, void *buffer, uint32_t length, uint32_t idle_timeout_ms, uint32_t total_timeout_ms);
int64_t http_socket_send_all(socket_handle_t socket,
                            const void *buffer,
                            uint32_t length,
                            uint32_t max_chunk,
                            uint32_t idle_timeout_ms,
                            uint32_t total_timeout_ms,
                            HTTPSocketTimeoutState *timeout);
void http_socket_fill_log_endpoints(socket_handle_t socket, netlog_socket_event_t *event);

#ifdef __cplusplus
}
#endif
