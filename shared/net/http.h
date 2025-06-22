#pragma once

#include "tcp.h"
#include "types.h"
#include "network_types.h"

typedef enum HTTPRequest {
    GET,
    POST,
    PUT,
    DELETE
} HTTPRequest;

sizedptr request_http_data(HTTPRequest request, network_connection_ctx *dest, uint16_t port);