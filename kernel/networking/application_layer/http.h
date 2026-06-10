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
    HTTP_METHOD_DELETE,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_OPTIONS
} HTTPMethod;

#define HTTP_METHOD_MASK_GET (1u << HTTP_METHOD_GET)
#define HTTP_METHOD_MASK_POST (1u << HTTP_METHOD_POST)
#define HTTP_METHOD_MASK_PUT (1u << HTTP_METHOD_PUT)
#define HTTP_METHOD_MASK_DELETE (1u << HTTP_METHOD_DELETE)
#define HTTP_METHOD_MASK_HEAD (1u << HTTP_METHOD_HEAD)
#define HTTP_METHOD_MASK_OPTIONS (1u << HTTP_METHOD_OPTIONS)
#define HTTP_METHOD_MASK_ALL (HTTP_METHOD_MASK_GET | HTTP_METHOD_MASK_POST | HTTP_METHOD_MASK_PUT | HTTP_METHOD_MASK_DELETE | HTTP_METHOD_MASK_HEAD | HTTP_METHOD_MASK_OPTIONS)

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
    HTTP_PARSE_UNKNOWN_METHOD,
    HTTP_PARSE_INCOMPLETE
} HTTPParseResult;

typedef enum {
    HTTP_CONTINUE = 100,
    HTTP_OK = 200,
    HTTP_PARTIAL_CONTENT = 206,
    HTTP_MOVED_PERMANENTLY = 301,
    HTTP_FOUND = 302,
    HTTP_SEE_OTHER = 303,
    HTTP_TEMPORARY_REDIRECT = 307,
    HTTP_PERMANENT_REDIRECT = 308,
    HTTP_BAD_REQUEST = 400,
    HTTP_UNAUTHORIZED = 401,
    HTTP_FORBIDDEN = 403,
    HTTP_NOT_FOUND = 404,
    HTTP_METHOD_NOT_ALLOWED = 405,
    HTTP_PAYLOAD_TOO_LARGE = 413,
    HTTP_URI_TOO_LONG = 414,
    HTTP_RANGE_NOT_SATISFIABLE = 416,
    HTTP_EXPECTATION_FAILED = 417,
    HTTP_HEADER_FIELDS_TOO_LARGE = 431,
    HTTP_INTERNAL_SERVER_ERROR = 500,
    HTTP_NOT_IMPLEMENTED = 501,
    HTTP_SERVICE_UNAVAILABLE = 503,
    HTTP_VERSION_NOT_SUPPORTED = 505,
} HttpError;

typedef enum {
    HTTP_HEADER_BUILD_REQUEST,
    HTTP_HEADER_BUILD_RESPONSE
} HTTPHeaderBuildKind;

typedef enum {
    HTTP_CHUNK_STAGE_SIZE,
    HTTP_CHUNK_STAGE_DATA,
    HTTP_CHUNK_STAGE_DATA_CRLF,
    HTTP_CHUNK_STAGE_TRAILERS,
    HTTP_CHUNK_STAGE_DONE
} HTTPChunkStage;

typedef enum {
    HTTP_POLICY_OPT_MAX_START_LINE = 1u << 0,
    HTTP_POLICY_OPT_MAX_HEADER_BYTES = 1u << 1,
    HTTP_POLICY_OPT_MAX_HEADER_COUNT = 1u << 2,
    HTTP_POLICY_OPT_MAX_HEADER_KEY_LEN = 1u << 3,
    HTTP_POLICY_OPT_MAX_HEADER_VALUE_LEN = 1u << 4,
    HTTP_POLICY_OPT_MAX_PATH_LEN = 1u << 5,
    HTTP_POLICY_OPT_MAX_BODY_BYTES = 1u << 6,
    HTTP_POLICY_OPT_HEADER_IDLE_TIMEOUT_MS = 1u << 7,
    HTTP_POLICY_OPT_HEADER_TOTAL_TIMEOUT_MS = 1u << 8,
    HTTP_POLICY_OPT_BODY_IDLE_TIMEOUT_MS = 1u << 9,
    HTTP_POLICY_OPT_BODY_TOTAL_TIMEOUT_MS = 1u << 10,
    HTTP_POLICY_OPT_ALLOW_CHUNKED = 1u << 11
} HTTPPolicyOptionFlag;

typedef enum {
    HTTP_SERVER_OPT_MAX_KEEPALIVE_REQUESTS = 1u << 0,
    HTTP_SERVER_OPT_ALLOWED_METHODS = 1u << 1,
    HTTP_SERVER_OPT_ERROR_CONTENT_TYPE = 1u << 2,
    HTTP_SERVER_OPT_ALLOW_KEEP_ALIVE = 1u << 3,
    HTTP_SERVER_OPT_ALLOW_ABSOLUTE_URI = 1u << 4,
    HTTP_SERVER_OPT_REQUIRE_HOST_HTTP11 = 1u << 5,
    HTTP_SERVER_OPT_SEND_ERROR_BODY = 1u << 6
} HTTPServerPolicyOptionFlag;

typedef enum {
    HTTP_CLIENT_OPT_MAX_REDIRECTS = 1u << 0,
    HTTP_CLIENT_OPT_FOLLOW_REDIRECTS = 1u << 1,
    HTTP_CLIENT_OPT_ALLOW_CLOSE_DELIMITED = 1u << 2
} HTTPClientPolicyOptionFlag;

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
} HTTPPolicy;

typedef struct {
    HTTPPolicy value;
    uint32_t flags;
} HTTPPolicyOptions;

typedef struct {
    HTTPPolicy common;
    uint32_t max_keepalive_requests;
    uint32_t allowed_methods;
    const char *error_content_type;
    bool allow_keep_alive;
    bool allow_absolute_uri;
    bool require_host_http11;
    bool send_error_body;
} HTTPServerPolicy;

typedef struct {
    HTTPServerPolicy value;
    uint32_t flags;
    uint32_t common_flags;
} HTTPServerPolicyOptions;

typedef struct {
    HTTPPolicy common;
    uint32_t max_redirects;
    bool follow_redirects;
    bool allow_close_delimited;
} HTTPClientPolicy;

typedef struct {
    HTTPClientPolicy value;
    uint32_t flags;
    uint32_t common_flags;
} HTTPClientPolicyOptions;

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
    string content_type;
    string connection;
    string host;
    string expect;
    string range;
    string location;
    string content_range;
    uint32_t content_length;
} HTTPHeaderFields;

typedef struct {
    uint8_t has_content_length;
    uint8_t chunked;
    uint8_t connection_close;
    uint8_t connection_keep_alive;
    uint8_t expect_continue;
} HTTPMessageFraming;

typedef struct {
    uint8_t has;
    uint8_t invalid;
    uint8_t has_start;
    uint8_t has_end;
    uint64_t start;
    uint64_t end;
} HTTPRangeSpec;

typedef struct {
    HTTPHeaderFields fields;
    HTTPMessageFraming framing;
    HTTPRangeSpec range;
    uint8_t bad_content_length;
} HTTPHeadersCommon;

typedef struct {
    HTTPMethod method;
    HTTPVersion version;
    string path;
    const char *host_override;
    HTTPHeadersCommon headers_common;
    HTTPHeader *extra_headers;
    uint32_t extra_header_count;
    string body;
} HTTPRequestMsg;

typedef struct {
    HttpError status_code;
    string reason;
    HTTPHeadersCommon headers_common;
    HTTPHeader *extra_headers;
    uint32_t extra_header_count;
    string body;
} HTTPResponseMsg;

typedef struct {
    HTTPPolicy policy;
    HTTPChunkStage stage;
    uint64_t chunk_size;
    uint64_t chunk_read;
    uint32_t body_total;
    uint8_t crlf_seen;
    string line;
    string body;
    string trailers_buf;
} HTTPChunkedDecoder;

HTTPPolicy http_default_policy(void);
HTTPPolicy http_policy_apply_options(HTTPPolicy base, const HTTPPolicyOptions *options);
HTTPServerPolicy http_server_policy_from_options(const HTTPServerPolicyOptions *options);
HTTPClientPolicy http_client_policy_from_options(const HTTPClientPolicyOptions *options);
const char* http_method_name(HTTPMethod method);
bool http_method_allowed(uint32_t mask, HTTPMethod method);
string http_methods_allow_header(uint32_t mask);
const char* http_status_reason(HttpError status);
HttpError http_parse_result_status(HTTPParseResult result);
bool http_header_value_has_token(const char *buf, uint32_t len, const char *token, uint32_t token_len);
HTTPParseResult http_parse_request_line(const char *buf, uint32_t len, HTTPRequestLine *out);
HTTPParseResult http_parse_status_line(const char *buf, uint32_t len, HTTPStatusLine *out);
string http_header_builder(const HTTPHeadersCommon *common,
                           const HTTPHeader *extra,
                           uint32_t extra_count,
                           HTTPHeaderBuildKind kind,
                           HTTPMethod method,
                           uint32_t status_code);

HTTPParseResult http_header_parse(const char *buf, uint32_t len,
                        const HTTPPolicy *policy,
                        HTTPHeadersCommon *out_common,
                        HTTPHeader **out_extra,
                        uint32_t *out_extra_count);

void http_chunked_decoder_init(HTTPChunkedDecoder *dec, const HTTPPolicy *policy);
HTTPParseResult http_chunked_decoder_feed(HTTPChunkedDecoder *dec, const char *buf, uint32_t len, uint32_t *out_used);
void http_chunked_decoder_free(HTTPChunkedDecoder *dec);

void http_headers_common_free(HTTPHeadersCommon *common);
void http_headers_extra_free(HTTPHeader *extra, uint32_t extra_count);
void http_request_free(HTTPRequestMsg *req);
void http_response_free(HTTPResponseMsg *res);

string http_request_builder(const HTTPRequestMsg *req);

string http_response_builder(const HTTPResponseMsg *res);

int find_crlfcrlf(const char *data, uint32_t len);

#ifdef __cplusplus
}
#endif
