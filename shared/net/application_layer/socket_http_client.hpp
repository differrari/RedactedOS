#pragma once
#include "console/kio.h"
#include "net/transport_layer/socket_tcp.hpp"
#include "http.h"
#include "std/string.h"
#include "std/memfunctions.h"
#define KP(fmt, ...) \
    do { kprintf(fmt, ##__VA_ARGS__); } while (0)

class HTTPClient {
private:
    TCPSocket sock;
    
public:
    explicit HTTPClient(uint16_t pid);
    ~HTTPClient();
    int32_t connect(uint32_t ip, uint16_t port);
    HTTPResponseMsg send_request(const HTTPRequestMsg &req);
    int32_t close();
};

HTTPClient::HTTPClient(uint16_t pid)
    : sock(SOCK_ROLE_CLIENT, pid)
{}

HTTPClient::~HTTPClient() {
    sock.close();
}

int32_t HTTPClient::connect(uint32_t ip, uint16_t port) {
    return sock.connect(ip, port);
}

HTTPResponseMsg HTTPClient::send_request(const HTTPRequestMsg &req) {
    string out = http_request_builder(&req);
    int64_t sent = sock.send(out.data, out.length);
    free(out.data, out.mem_length);

    HTTPResponseMsg resp{};
    if (sent < 0) {
        resp.status_code = (HttpError)sent;
        return resp;
    }

    string buf = string_repeat('\0', 0);
    char tmp[512];
    int attempts = 0;
    int hdr_end = -1;
    while (true) {
        int64_t r = sock.recv(tmp, sizeof(tmp));
        if (r < 0) {
            free(buf.data, buf.mem_length);
            resp.status_code = (HttpError)SOCK_ERR_SYS;
            return resp;
        }
        if (r > 0) {
            string_append_bytes(&buf, tmp, (uint32_t)r);
        }
        hdr_end = find_crlfcrlf(buf.data, buf.length);
        if (hdr_end >= 0) break;
        if (++attempts > 50) {
            free(buf.data, buf.mem_length);
            resp.status_code = (HttpError)SOCK_ERR_PROTO;
            return resp;
        }
        sleep(10);
    }

    {
        uint32_t i = 0;
        while (i < (uint32_t)hdr_end && buf.data[i] != ' ') i++;
        uint32_t code = 0, j = i+1;
        while (j < (uint32_t)hdr_end && buf.data[j] >= '0' && buf.data[j] <= '9') {
            code = code*10 + (buf.data[j]-'0'); ++j;
        }
        resp.status_code = (HttpError)code;
        while (j < (uint32_t)hdr_end && buf.data[j]==' ') ++j;
        if (j < (uint32_t)hdr_end) {
            uint32_t rlen = hdr_end - j;
            resp.reason = string_repeat('\0', 0);
            string_append_bytes(&resp.reason, buf.data+j, rlen);
        }
    }

    HTTPHeader *extras = nullptr;
    uint32_t extra_count = 0;
    int status_line_end = strindex((char*)buf.data, "\r\n");
    http_header_parser(
        (char*)buf.data + status_line_end + 2,
        buf.length - (uint32_t)(status_line_end + 2),
        &resp.headers_common,
        &extras,
        &extra_count);
    resp.extra_headers = extras;
    resp.extra_header_count = extra_count;

    uint32_t body_start = hdr_end + 4;  
    uint32_t have = (buf.length > body_start)
                ? buf.length - body_start
                : 0;

    uint32_t need = resp.headers_common.length;
    if (need > 0) {
        while (have < need) {
            int64_t r = sock.recv(tmp, sizeof(tmp));
            if (r <= 0) break;
            string_append_bytes(&buf, tmp, (uint32_t)r);
            have += (uint32_t)r;
        }
    } else {
        int idle = 0;
        while (idle < 5) {
            int64_t r = sock.recv(tmp, sizeof(tmp));
            if (r > 0) {
                string_append_bytes(&buf, tmp, (uint32_t)r);
                have += (uint32_t)r;
                idle = 0;
            } else {
                ++idle;
                sleep(20);
            }
        }
    }
    if (have > 0) {
        char *body_copy = (char*)malloc(have + 1);
        if (body_copy) {
            memcpy(body_copy,
                   buf.data + body_start,
                   have);
            body_copy[have] = '\0';
            resp.body.ptr  = (uintptr_t)body_copy;
            resp.body.size = have;
        }
    }
    free(buf.data, buf.mem_length);
    return resp;
}

int32_t HTTPClient::close() {
    return sock.close();
}
