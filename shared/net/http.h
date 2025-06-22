#pragma once

#include "tcp.h"
#include "types.h"
#include "network_types.h"
#include "std/string.h"

typedef enum HTTPRequest {
    GET,
    POST,
    PUT,
    DELETE
} HTTPRequest;

sizedptr request_http_data(HTTPRequest request, network_connection_ctx *dest, uint16_t port);
sizedptr http_get_payload(sizedptr header);
string http_get_chunked_payload(sizedptr chunk);