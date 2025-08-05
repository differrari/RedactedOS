#pragma once

#include "std/string.h"
#include "std/memfunctions.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE
} HTTPMethod;

typedef enum {
    HTTP_OK = 200,
    HTTP_BAD_REQUEST = 400,
    HTTP_UNAUTHORIZED = 401,
    HTTP_FORBIDDEN = 403,
    HTTP_NOT_FOUND = 404,
    HTTP_INTERNAL_SERVER_ERROR = 500,
    HTTP_NOT_IMPLEMENTED = 501,
    HTTP_SERVICE_UNAVAILABLE = 503,
    HTTP_DEBUG = 800,
} HttpError;

typedef struct {
    string key;
    string value;
} HTTPHeader;

typedef struct {
    uint32_t length;
    string type;
    string date;
    string connection;
    string keep_alive;
    string host;
} HTTPHeadersCommon;

typedef struct {
    HTTPMethod method;
    string path;
    HTTPHeadersCommon headers_common;
    HTTPHeader *extra_headers;
    uint32_t extra_header_count;
    sizedptr body;
} HTTPRequestMsg;

typedef struct {
    HttpError status_code;
    string reason;
    HTTPHeadersCommon headers_common;
    HTTPHeader *extra_headers;
    uint32_t extra_header_count;
    sizedptr  body;
} HTTPResponseMsg;

string http_header_builder(const HTTPHeadersCommon *common,
                           const HTTPHeader *extra,
                           uint32_t extra_count);

void http_header_parser(const char *buf, uint32_t len,
                        HTTPHeadersCommon *out_common,
                        HTTPHeader **out_extra,
                        uint32_t *out_extra_count);

string http_request_builder(const HTTPRequestMsg *req);

string http_response_builder(const HTTPResponseMsg *res);

int find_crlfcrlf(const char *data, uint32_t len);

sizedptr http_get_payload(sizedptr header);

string http_get_chunked_payload(sizedptr chunk);

#ifdef __cplusplus
}
#endif
