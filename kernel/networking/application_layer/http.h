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
#define HTTP_DEFAULT_MAX_KEEPALIVE_REQUESTS 16
#define HTTP_DEFAULT_MAX_REDIRECTS 5

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
    HTTP_PARSE_UNSUPPORTED_METHOD,
    HTTP_PARSE_INCOMPLETE
} HTTPParseResult;

typedef enum {
    HTTP_OK = 200,
    HTTP_MOVED_PERMANENTLY = 301,
    HTTP_FOUND = 302,
    HTTP_SEE_OTHER = 303,
    HTTP_TEMPORARY_REDIRECT = 307,
    HTTP_PERMANENT_REDIRECT = 308,
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
    uint32_t max_keepalive_requests;
    uint32_t max_redirects;
    bool allow_chunked;
    bool allow_keep_alive;
    bool allow_absolute_uri;
    bool require_host_http11;
} HTTPPolicy;

typedef struct {
    int32_t max_start_line;
    int32_t max_header_bytes;
    int32_t max_header_count;
    int32_t max_header_key_len;
    int32_t max_header_value_len;
    int32_t max_path_len;
    int32_t max_body_bytes;
    int32_t header_idle_timeout_ms;
    int32_t header_total_timeout_ms;
    int32_t body_idle_timeout_ms;
    int32_t body_total_timeout_ms;
    int32_t max_keepalive_requests;
    int32_t max_redirects;
    int32_t allow_chunked;
    int32_t allow_keep_alive;
    int32_t allow_absolute_uri;
    int32_t require_host_http11;
} HTTPPolicyOptions;

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
    uint8_t connection_close;
    uint8_t connection_keep_alive;
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
    HTTPVersion version;
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
HTTPPolicy http_policy_from_options(const HTTPPolicyOptions *options);
const char* http_status_reason(HttpError status);
HttpError http_parse_result_status(HTTPParseResult result);
bool http_header_value_has_token(const char *buf, uint32_t len, const char *token, uint32_t token_len);
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

HTTPParseResult http_decode_chunked_body(const char *buf, uint32_t len,
                                    const HTTPPolicy *policy,
                                    string *out_body,
                                    uint32_t *out_used);

void http_headers_common_free(HTTPHeadersCommon *common);
void http_headers_extra_free(HTTPHeader *extra, uint32_t extra_count);

string http_request_builder(const HTTPRequestMsg *req);

string http_response_builder(const HTTPResponseMsg *res);

int find_crlfcrlf(const char *data, uint32_t len);

sizedptr http_get_payload(sizedptr header);

#ifdef __cplusplus
}
#endif
