#include "csocket_http_internal.h"
#include "http.h"
#include "net/socket_types.h"
#include "process/scheduler.h"
#include "syscalls/syscalls.h"

int32_t http_socket_close(socket_handle_t *socket, bool abort) {
    if (!socket || !*socket) return SOCK_ERR_STATE;

    socket_handle_t handle = *socket;
    if (abort) {
        SocketLinger linger = {.enabled = 1, .timeout_ms = 0};
        set_socket_option(handle, SOCK_OPT_LINGER, &linger, sizeof(linger));
    } else {
        uint32_t nonblock = 0;
        set_socket_option(handle, SOCK_OPT_NONBLOCK, &nonblock, sizeof(nonblock));
    }

    int32_t close_result = close_socket(handle);
    if (close_result != SOCK_ERR_WOULDBLOCK) *socket = 0;
    return close_result;
}

int64_t http_socket_recv_wait(socket_handle_t socket,
                            void *buffer,
                            uint32_t length, 
                            uint32_t idle_timeout_ms,
                            uint32_t total_timeout_ms,
                            HTTPSocketTimeoutState *timeout) {
    while (1) {
        uint32_t now = (uint32_t)get_time();
        if ((idle_timeout_ms && now - timeout->last_progress_ms >= idle_timeout_ms)|| (total_timeout_ms && now - timeout->start_ms >= total_timeout_ms)) return SOCK_ERR_WOULDBLOCK;

        int64_t result = receive_from_socket(socket, buffer, length, NULL);
        now = (uint32_t)get_time();
        if (result == SOCK_ERR_WOULDBLOCK) {
            if ((idle_timeout_ms && now - timeout->last_progress_ms >= idle_timeout_ms) || (total_timeout_ms && now - timeout->start_ms >= total_timeout_ms)) return SOCK_ERR_WOULDBLOCK;
            msleep(2);
            continue;
        }
        if (result > 0) timeout->last_progress_ms = now;
        return result;
    }
}

int32_t http_socket_recv_head(socket_handle_t socket,
                            string *buffer,
                            uint32_t max_header_bytes,
                            uint32_t idle_timeout_ms,
                            uint32_t total_timeout_ms,
                            int32_t *header_end) {
    *header_end = find_crlfcrlf(buffer->data, buffer->length);
    if (*header_end >= 0) return (uint32_t)*header_end + 4 <= max_header_bytes ? SOCK_OK : SOCK_ERR_PROTO;
    if (buffer->length >= max_header_bytes) return SOCK_ERR_PROTO;

    uint32_t now = (uint32_t)get_time();
    HTTPSocketTimeoutState timeout = {now, now};
    char temporary[2048];

    while (*header_end < 0) {
        uint32_t available = max_header_bytes - buffer->length;
        uint32_t request = available < sizeof(temporary) ? available : sizeof(temporary);
        int64_t result = http_socket_recv_wait(socket, temporary, request, idle_timeout_ms, total_timeout_ms, &timeout);
        if (result < 0) return (int32_t)result;
        if (result == 0) return SOCK_ERR_PROTO;

        uint32_t expected = buffer->length + (uint32_t)result;
        string_append_bytes(buffer, temporary, (uint32_t)result);
        if (buffer->length != expected) return SOCK_ERR_SYS;

        *header_end = find_crlfcrlf(buffer->data, buffer->length);
        if (*header_end >= 0) return (uint32_t)*header_end + 4 <= max_header_bytes ? SOCK_OK : SOCK_ERR_PROTO;
        if (buffer->length >= max_header_bytes) return SOCK_ERR_PROTO;
    }

    return SOCK_OK;
}

int64_t http_socket_recv_exact(socket_handle_t socket, void *buffer, uint32_t length, uint32_t idle_timeout_ms, uint32_t total_timeout_ms) {
    uint32_t now = (uint32_t)get_time();
    HTTPSocketTimeoutState timeout = {now, now};
    uint32_t received = 0;

    while (received < length) {
        int64_t result = http_socket_recv_wait(socket, (uint8_t*)buffer + received, length - received, idle_timeout_ms, total_timeout_ms, &timeout);
        if (result < 0) return result;
        if (result == 0) return SOCK_ERR_PROTO;
        if ((uint64_t)result > length - received) return SOCK_ERR_STATE;
        received += (uint32_t)result;
    }

    return received;
}

int64_t http_socket_send_all(socket_handle_t socket,
                            const void *buffer,
                            uint32_t length,
                            uint32_t max_chunk,
                            uint32_t idle_timeout_ms,
                            uint32_t total_timeout_ms,
                            HTTPSocketTimeoutState *timeout) {
    uint32_t sent = 0;
    while (sent < length) {
        uint32_t now = (uint32_t)get_time();
        if ((idle_timeout_ms && now - timeout->last_progress_ms >= idle_timeout_ms) || (total_timeout_ms && now - timeout->start_ms >= total_timeout_ms)) return SOCK_ERR_WOULDBLOCK;

        uint32_t request = length - sent;
        if (max_chunk && request > max_chunk) request = max_chunk;

        int64_t result = send_on_socket(socket, (const uint8_t*)buffer + sent, request);
        now = (uint32_t)get_time();
        if (result == SOCK_ERR_WOULDBLOCK || result == 0) {
            if ((idle_timeout_ms && now - timeout->last_progress_ms >= idle_timeout_ms) || (total_timeout_ms && now - timeout->start_ms >= total_timeout_ms)) return SOCK_ERR_WOULDBLOCK;
            msleep(2);
            continue;
        }
        if (result < 0) return result;
        if ((uint64_t)result > request) return SOCK_ERR_STATE;

        sent += (uint32_t)result;
        timeout->last_progress_ms = now;
    }

    return sent;
}

void http_socket_fill_log_endpoints(socket_handle_t socket, netlog_socket_event_t *event) {
    uint32_t local_port = 0;
    uint32_t length = sizeof(local_port);
    if (get_socket_option(socket, SOCK_GET_LOCAL_PORT, &local_port, &length) == SOCK_OK) event->local_port = local_port;
    length = sizeof(event->remote_ep);
    get_socket_option(socket, SOCK_GET_REMOTE_ENDPOINT, &event->remote_ep, &length);
}
