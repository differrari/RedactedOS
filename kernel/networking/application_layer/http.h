#pragma once

#include "std/string.h"
#include "std/memory.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_DEFAULT_MAX_START_LINE 2048
#define HTTP_DEFAULT_MAX_HEADER_BYTES (16 * 1024)
#define HTTP_DEFAULT_MAX_HEADER_COUNT 64
#define HTTP_DEFAULT_MAX_HEADER_KEY_LEN 64
#define HTTP_DEFAULT_MAX_HEADER_VALUE_LEN 4096
#define HTTP_DEFAULT_MAX_PATH_LEN 2048
#define HTTP_DEFAULT_MAX_BODY_BYTES (1024*1024)
#define HTTP_DEFAULT_HEADER_IDLE_TIMEOUT_MS 1000
#define HTTP_DEFAULT_HEADER_TOTAL_TIMEOUT_MS 15000
#define HTTP_DEFAULT_BODY_IDLE_TIMEOUT_MS 3000
#define HTTP_DEFAULT_BODY_TOTAL_TIMEOUT_MS 20000

typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE
} HTTPMethod;

typedef enum {
    HTTP_VERSION_UNKNOWN,
    HTTP_VERSION_10,
    HTTP_VERSION_11
} HTTPVersion;

typedef enum {
    HTTP_PARSE_OK,
    HTTP_PARSE_BAD_FORMAT,
    HTTP_PARSE_TOO_LARGE,
    HTTP_PARSE_TOO_MANY_HEADERS,
    HTTP_PARSE_BAD_CONTENT_LENGTH,
    HTTP_PARSE_UNSUPPORTED_TRANSFER,
    HTTP_PARSE_MISSING_HOST,
    HTTP_PARSE_UNSUPPORTED_VERSION,
    HTTP_PARSE_PAYLOAD_TOO_LARGE,
    HTTP_PARSE_UNSUPPORTED_METHOD
} HTTPParseResult;

typedef enum {
    HTTP_OK = 200,
    HTTP_BAD_REQUEST = 400,
    HTTP_UNAUTHORIZED = 401,
    HTTP_FORBIDDEN = 403,
    HTTP_NOT_FOUND = 404,
    HTTP_PAYLOAD_TOO_LARGE = 413,
    HTTP_URI_TOO_LONG = 414,
    HTTP_EXPECTATION_FAILED = 417,
    HTTP_HEADER_FIELDS_TOO_LARGE = 431,
    HTTP_INTERNAL_SERVER_ERROR = 500,
    HTTP_NOT_IMPLEMENTED = 501,
    HTTP_SERVICE_UNAVAILABLE = 503,
    HTTP_VERSION_NOT_SUPPORTED = 505,
    HTTP_DEBUG = 800,
} HttpError;

typedef struct {
    uint32_t max_start_line;
    uint32_t max_header_bytes;
    uint32_t max_header_count;
    uint32_t max_header_key_len;
    uint32_t max_header_value_len;
    uint32_t max_path_len;
    uint32_t max_body_bytes;
    uint32_t header_idle_timeout_ms;
    uint32_t header_total_timeout_ms;
    uint32_t body_idle_timeout_ms;
    uint32_t body_total_timeout_ms;
    bool allow_chunked;
    bool allow_keep_alive;
    bool allow_absolute_uri;
    bool require_host_http11;
} HTTPPolicy;

typedef struct {
    HTTPMethod method;
    HTTPVersion version;
    uint32_t target_off;
    uint32_t target_len;
} HTTPRequestLine;

typedef struct {
    HTTPVersion version;
    uint32_t status_code;
    uint32_t reason_off;
    uint32_t reason_len;
} HTTPStatusLine;

typedef struct {
    string key;
    string value;
} HTTPHeader;

typedef struct {
    uint32_t length;
    uint8_t has_length;
    uint8_t bad_length;
    uint8_t has_transfer_encoding;
    uint8_t chunked;
    uint8_t bad_transfer;
    string type;
    string date;
    string connection;
    string keep_alive;
    string host;
    string content_type;
    string transfer_encoding;
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
    sizedptr body;
} HTTPResponseMsg;

HTTPPolicy http_default_policy(void);
const char* http_status_reason(HttpError status);
HttpError http_parse_result_status(HTTPParseResult result);
HTTPParseResult http_parse_request_line(const char *buf, uint32_t len, HTTPRequestLine *out);
HTTPParseResult http_parse_status_line(const char *buf, uint32_t len, HTTPStatusLine *out);
string http_header_builder(const HTTPHeadersCommon *common,
                           const HTTPHeader *extra,
                           uint32_t extra_count);

HTTPParseResult http_header_parse_policy(const char *buf, uint32_t len,
                        const HTTPPolicy *policy,
                        HTTPHeadersCommon *out_common,
                        HTTPHeader **out_extra,
                        uint32_t *out_extra_count);

void http_headers_common_free(HTTPHeadersCommon *common);
void http_headers_extra_free(HTTPHeader *extra, uint32_t extra_count);

string http_request_builder(const HTTPRequestMsg *req);

string http_response_builder(const HTTPResponseMsg *res);

int find_crlfcrlf(const char *data, uint32_t len);

sizedptr http_get_payload(sizedptr header);

string http_get_chunked_payload(sizedptr chunk);

#ifdef __cplusplus
}
#endif
