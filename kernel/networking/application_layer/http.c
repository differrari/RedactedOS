#include "http.h"
#include "std/string.h"
#include "std/memory.h"

HTTPPolicy http_default_policy(void) {
    HTTPPolicy p = {
        .max_start_line = HTTP_DEFAULT_MAX_START_LINE,
        .max_header_bytes = HTTP_DEFAULT_MAX_HEADER_BYTES,
        .max_header_count = HTTP_DEFAULT_MAX_HEADER_COUNT,
        .max_header_key_len = HTTP_DEFAULT_MAX_HEADER_KEY_LEN,
        .max_header_value_len = HTTP_DEFAULT_MAX_HEADER_VALUE_LEN,
        .max_path_len = HTTP_DEFAULT_MAX_PATH_LEN,
        .max_body_bytes = HTTP_DEFAULT_MAX_BODY_BYTES,
        .header_idle_timeout_ms = HTTP_DEFAULT_HEADER_IDLE_TIMEOUT_MS,
        .header_total_timeout_ms = HTTP_DEFAULT_HEADER_TOTAL_TIMEOUT_MS,
        .body_idle_timeout_ms = HTTP_DEFAULT_BODY_IDLE_TIMEOUT_MS,
        .body_total_timeout_ms = HTTP_DEFAULT_BODY_TOTAL_TIMEOUT_MS,
        .allow_chunked = true,
    };
    return p;
}

HTTPPolicy http_policy_from_options(const HTTPPolicyOptions *options) {
    HTTPPolicy p = http_default_policy();
    if (!options) return p;

    if (options->max_start_line > 0) p.max_start_line = options->max_start_line;
    if (options->max_header_bytes > 0) p.max_header_bytes = options->max_header_bytes;
    if (options->max_header_count > 0) p.max_header_count = options->max_header_count;
    if (options->max_header_key_len > 0) p.max_header_key_len = options->max_header_key_len;
    if (options->max_header_value_len > 0) p.max_header_value_len = options->max_header_value_len;
    if (options->max_path_len > 0) p.max_path_len = options->max_path_len;
    if (options->max_body_bytes > 0) p.max_body_bytes = options->max_body_bytes;
    if (options->header_idle_timeout_ms > 0) p.header_idle_timeout_ms = options->header_idle_timeout_ms;
    if (options->header_total_timeout_ms > 0) p.header_total_timeout_ms = options->header_total_timeout_ms;
    if (options->body_idle_timeout_ms > 0) p.body_idle_timeout_ms = options->body_idle_timeout_ms;
    if (options->body_total_timeout_ms > 0) p.body_total_timeout_ms = options->body_total_timeout_ms;
    if (options->allow_chunked > 0) p.allow_chunked = true;
    else if (options->allow_chunked < 0) p.allow_chunked = false;
    return p;
}

HTTPServerPolicy http_server_policy_from_options(const HTTPServerPolicyOptions *options) {
    HTTPServerPolicy p = {
        .common = http_default_policy(),
        .max_keepalive_requests = HTTP_DEFAULT_MAX_KEEPALIVE_REQUESTS,
        .allow_keep_alive = true,
        .allow_absolute_uri = true,
        .require_host_http11 = true
    };

    if (!options) return p;

    p.common = http_policy_from_options(&options->common);
    if (options->max_keepalive_requests > 0) p.max_keepalive_requests = options->max_keepalive_requests;
    if (options->allow_keep_alive > 0) p.allow_keep_alive = true;
    else if (options->allow_keep_alive < 0) p.allow_keep_alive = false;
    if (options->allow_absolute_uri > 0) p.allow_absolute_uri = true;
    else if (options->allow_absolute_uri < 0) p.allow_absolute_uri = false;
    if (options->require_host_http11 > 0) p.require_host_http11 = true;
    else if (options->require_host_http11 < 0) p.require_host_http11 = false;
    return p;
}

HTTPClientPolicy http_client_policy_from_options(const HTTPClientPolicyOptions *options) {
    HTTPClientPolicy p = {
        .common = http_default_policy(),
        .max_redirects = HTTP_DEFAULT_MAX_REDIRECTS,
        .follow_redirects = false,
        .allow_close_delimited = true
    };

    if (!options) return p;

    p.common = http_policy_from_options(&options->common);
    if (options->max_redirects > 0) p.max_redirects = options->max_redirects;
    if (options->follow_redirects > 0) p.follow_redirects = true;
    else if (options->follow_redirects < 0) p.follow_redirects = false;
    if (options->allow_close_delimited > 0) p.allow_close_delimited = true;
    else if (options->allow_close_delimited < 0) p.allow_close_delimited = false;
    return p;
}

const char* http_method_name(HTTPMethod method) {
    switch (method) {
        case HTTP_METHOD_GET: return "GET";
        case HTTP_METHOD_POST: return "POST";
        case HTTP_METHOD_PUT: return "PUT";
        case HTTP_METHOD_DELETE: return "DELETE";
        case HTTP_METHOD_HEAD: return "HEAD";
        case HTTP_METHOD_OPTIONS: return "OPTIONS";
        default: return "GET";
    }
}

const char* http_status_reason(HttpError status) {
    switch (status) {
        case HTTP_CONTINUE: return "Continue";
        case HTTP_OK: return "OK";
        case HTTP_PARTIAL_CONTENT: return "Partial Content";
        case HTTP_MOVED_PERMANENTLY: return "Moved Permanently";
        case HTTP_FOUND: return "Found";
        case HTTP_SEE_OTHER: return "See Other";
        case HTTP_TEMPORARY_REDIRECT: return "Temporary Redirect";
        case HTTP_PERMANENT_REDIRECT: return "Permanent Redirect";
        case HTTP_BAD_REQUEST: return "Bad Request";
        case HTTP_UNAUTHORIZED: return "Unauthorized";
        case HTTP_FORBIDDEN: return "Forbidden";
        case HTTP_NOT_FOUND: return "Not Found";
        case HTTP_PAYLOAD_TOO_LARGE: return "Payload Too Large";
        case HTTP_URI_TOO_LONG: return "URI Too Long";
        case HTTP_RANGE_NOT_SATISFIABLE: return "Range Not Satisfiable";
        case HTTP_EXPECTATION_FAILED: return "Expectation Failed";
        case HTTP_HEADER_FIELDS_TOO_LARGE: return "Request Header Fields Too Large";
        case HTTP_INTERNAL_SERVER_ERROR: return "Internal Server Error";
        case HTTP_NOT_IMPLEMENTED: return "Not Implemented";
        case HTTP_SERVICE_UNAVAILABLE: return "Service Unavailable";
        case HTTP_VERSION_NOT_SUPPORTED: return "HTTP Version Not Supported";
        default: return "Error";
    }
}

HttpError http_parse_result_status(HTTPParseResult result) {
    switch (result) {
        case HTTP_PARSE_TOO_LARGE:
        case HTTP_PARSE_TOO_MANY_HEADERS:
            return HTTP_HEADER_FIELDS_TOO_LARGE;
        case HTTP_PARSE_BAD_CONTENT_LENGTH:
        case HTTP_PARSE_BAD_FORMAT:
        case HTTP_PARSE_MISSING_HOST:
            return HTTP_BAD_REQUEST;
        case HTTP_PARSE_UNSUPPORTED_TRANSFER:
        case HTTP_PARSE_UNSUPPORTED_METHOD:
            return HTTP_NOT_IMPLEMENTED;
        case HTTP_PARSE_UNSUPPORTED_VERSION:
            return HTTP_VERSION_NOT_SUPPORTED;
        case HTTP_PARSE_PAYLOAD_TOO_LARGE:
            return HTTP_PAYLOAD_TOO_LARGE;
        case HTTP_PARSE_INCOMPLETE:
            return HTTP_BAD_REQUEST;
        default:
            return HTTP_BAD_REQUEST;
    }
}

bool http_header_value_has_token(const char *buf, uint32_t len, const char *token, uint32_t token_len) {
    if (!buf || !token || !token_len) return false;

    uint32_t pos = 0;
    while (pos < len) {
        while (pos < len && (is_whitespace(buf[pos]) || buf[pos] == ',')) pos++;
        uint32_t start = pos;
        while (pos < len && buf[pos] != ',') pos++;
        uint32_t end = pos;
        while (end > start && is_whitespace(buf[end - 1])) end--;
        if (end - start == token_len && strncmp_case(buf + start, token, true, token_len) == 0) return true;
        if (pos < len && buf[pos] == ',') pos++;
    }

    return false;
}

HTTPParseResult http_parse_request_line(const char *buf, uint32_t len, HTTPRequestLine *out) {
    if (!buf || !out || !len) return HTTP_PARSE_BAD_FORMAT;
    *out = (HTTPRequestLine){0};

    uint32_t i = 0;
    while (i < len && buf[i] != ' ') i++;
    uint32_t mlen = i;
    if (!mlen || i >= len) return HTTP_PARSE_BAD_FORMAT;

    if (mlen == 3 && memcmp(buf, "GET", 3) == 0) out->method = HTTP_METHOD_GET;
    else if (mlen == 4 && memcmp(buf, "POST", 4) == 0) out->method = HTTP_METHOD_POST;
    else if (mlen == 3 && memcmp(buf, "PUT", 3) == 0) out->method = HTTP_METHOD_PUT;
    else if (mlen == 6 && memcmp(buf, "DELETE", 6) == 0) out->method = HTTP_METHOD_DELETE;
    else if (mlen == 4 && memcmp(buf, "HEAD", 4) == 0) out->method = HTTP_METHOD_HEAD;
    else if (mlen == 7 && memcmp(buf, "OPTIONS", 7) == 0) out->method = HTTP_METHOD_OPTIONS;
    else return HTTP_PARSE_UNSUPPORTED_METHOD;

    i++;
    while (i < len && buf[i] == ' ')i++;
    uint32_t target_off = i;
    while (i < len && buf[i] != ' ')i++;
    uint32_t target_len = i - target_off;
    if (!target_len || i >= len) return HTTP_PARSE_BAD_FORMAT;

    i++;
    while (i < len && buf[i] == ' ') i++;
    uint32_t version_len = len - i;
    if (version_len != 8) return HTTP_PARSE_UNSUPPORTED_VERSION;
    if (memcmp(buf + i, "HTTP/1.0", 8) == 0) out->version = HTTP_VERSION_10;
    else if (memcmp(buf + i, "HTTP/1.1", 8) == 0) out->version = HTTP_VERSION_11;
    else return HTTP_PARSE_UNSUPPORTED_VERSION;

    out->target_off = target_off;
    out->target_len = target_len;
    return HTTP_PARSE_OK;
}

HTTPParseResult http_parse_status_line(const char *buf, uint32_t len, HTTPStatusLine *out) {
    if (!buf || !out || len < 12) return HTTP_PARSE_BAD_FORMAT;
    *out = (HTTPStatusLine){0};

    if (memcmp(buf, "HTTP/1.0", 8) == 0) out->version = HTTP_VERSION_10;
    else if (memcmp(buf, "HTTP/1.1", 8) == 0) out->version = HTTP_VERSION_11;
    else return HTTP_PARSE_UNSUPPORTED_VERSION;

    if (buf[8] != ' ') return HTTP_PARSE_BAD_FORMAT;
    char code_buf[4];
    memcpy(code_buf, buf + 9,3);
    code_buf[3] = 0;

    uint32_t code = 0;
    if (!parse_uint32_dec_exact(code_buf, &code)) return HTTP_PARSE_BAD_FORMAT;
    if (code < 100 || code > 999) return HTTP_PARSE_BAD_FORMAT;
    out->status_code = code;

    uint32_t i = 12;
    while (i < len && buf[i] == ' ') i++;
    out->reason_off = i;
    out->reason_len = len - i;
    return HTTP_PARSE_OK;
}

static void http_append_field(string *out, const char *key, uint32_t key_len, const char *val, uint32_t val_len) {
    if (!out || !key || !key_len || !val) return;
    string_append_bytes(out, key, key_len);
    string_append_bytes(out, ": ", 2);
    string_append_bytes(out, val, val_len);
    string_append_bytes(out, "\r\n", 2);
}

string http_header_builder(const HTTPHeadersCommon *C, const HTTPHeader *H, uint32_t N, HTTPHeaderBuildKind kind, HTTPMethod method, uint32_t status_code){
    string out = string_repeat('\0', 0);
    bool request = kind == HTTP_HEADER_BUILD_REQUEST;
    bool response = kind == HTTP_HEADER_BUILD_RESPONSE;
    bool informational = response && status_code >= 100 && status_code < 200;

    if (!C){
        for (uint32_t i = 0; H && i < N; i++) http_append_field(&out, H[i].key.data, H[i].key.length, H[i].value.data, H[i].value.length);
        string_append_bytes(&out, "\r\n", 2);
        return out;
    }

    if (C->fields.content_type.length && !informational) http_append_field(&out,"Content-Type", 12, C->fields.content_type.data, C->fields.content_type.length);
    if (!informational) {
        if (C->framing.chunked) string_append_bytes(&out, "Transfer-Encoding: chunked\r\n", 28);
        else if (C->framing.has_content_length || C->fields.content_length || response || method == HTTP_METHOD_POST || method == HTTP_METHOD_PUT) {
            string tmp = string_format("Content-Length: %i\r\n", (int)C->fields.content_length);
            string_append_bytes(&out, tmp.data, tmp.length);
            string_free(tmp);
        }
    }

    if (response && C->fields.location.length) http_append_field(&out, "Location", 8, C->fields.location.data, C->fields.location.length);
    if (request && C->fields.range.length) http_append_field(&out, "Range", 5, C->fields.range.data, C->fields.range.length);
    if (response && C->fields.content_range.length) http_append_field(&out, "Content-Range", 13, C->fields.content_range.data, C->fields.content_range.length);
    if (request && C->fields.expect.length) http_append_field(&out, "Expect", 6, C->fields.expect.data, C->fields.expect.length);
    
    if (request && C->fields.host.length){
        string_append_bytes(&out, "Host: ", 6);
        bool has_colon = str_has_char(C->fields.host.data, C->fields.host.length, ':') >= 0;
        bool has_lb = str_has_char(C->fields.host.data, C->fields.host.length, '[') >= 0;
        bool has_rb = str_has_char(C->fields.host.data, C->fields.host.length, ']') >= 0;
        if (has_colon && !has_lb && !has_rb){
            string_append_bytes(&out, "[", 1);
            string_append_bytes(&out, C->fields.host.data, C->fields.host.length);
            string_append_bytes(&out, "]", 1);
        } else {
            string_append_bytes(&out, C->fields.host.data, C->fields.host.length);
        }
        string_append_bytes(&out, "\r\n", 2);
    }

    if (C->fields.connection.length) http_append_field(&out, "Connection", 10, C->fields.connection.data, C->fields.connection.length);
    for (uint32_t i = 0; H && i < N; i++) http_append_field(&out, H[i].key.data, H[i].key.length, H[i].value.data, H[i].value.length);

    string_append_bytes(&out, "\r\n", 2);
    return out;
}

void http_headers_common_free(HTTPHeadersCommon *C){
    if (!C) return;
    if (C->fields.content_type.mem_length) string_free(C->fields.content_type);
    if (C->fields.connection.mem_length) string_free(C->fields.connection);
    if (C->fields.host.mem_length) string_free(C->fields.host);
    if (C->fields.expect.mem_length) string_free(C->fields.expect);
    if (C->fields.range.mem_length) string_free(C->fields.range);
    if (C->fields.location.mem_length) string_free(C->fields.location);
    if (C->fields.content_range.mem_length) string_free(C->fields.content_range);
    *C = (HTTPHeadersCommon){0};
}

void http_headers_extra_free(HTTPHeader *extra, uint32_t extra_count){
    if (!extra) return;
    for (uint32_t i = 0; i < extra_count; i++){
        if (extra[i].key.mem_length) string_free(extra[i].key);
        if (extra[i].value.mem_length) string_free(extra[i].value);
    }
    release(extra);
}

HTTPParseResult http_header_parse(const char *buf, uint32_t len, const HTTPPolicy *policy, HTTPHeadersCommon *C, HTTPHeader **out_extra, uint32_t *out_extra_count){
    HTTPPolicy p = policy ? *policy : http_default_policy();
    if (!buf || !C || !out_extra || !out_extra_count) return HTTP_PARSE_BAD_FORMAT;
    if (len > p.max_header_bytes) return HTTP_PARSE_TOO_LARGE;

    *C = (HTTPHeadersCommon){0};
    *out_extra = NULL;
    *out_extra_count = 0;

    uint32_t max_lines = 0;
    uint32_t count_pos = 0;
    while (count_pos < len) {
        uint32_t eol = count_pos;
        bool has_crlf = false;
        while (eol + 1 < len) {
            if (buf[eol] == '\r' && buf[eol+1] == '\n') {
                has_crlf = true;
                break;
            }
            eol++;
        }
        if (!has_crlf) eol = len;
        if (eol == count_pos) break;

        uint32_t sep = count_pos;
        while (sep < eol && buf[sep] != ':') sep++;
        if (sep == eol || sep == count_pos) return HTTP_PARSE_BAD_FORMAT;

        uint32_t key_len = sep - count_pos;
        uint32_t val_start = sep + 1;
        while (val_start < eol && is_whitespace(buf[val_start])) val_start++;
        uint32_t val_end = eol;
        while (val_end > val_start && is_whitespace(buf[val_end-1])) val_end--;

        if (key_len > p.max_header_key_len || key_len >= 128 || val_end - val_start > p.max_header_value_len) return HTTP_PARSE_TOO_LARGE;
        max_lines++;
        if (max_lines > p.max_header_count) return HTTP_PARSE_TOO_MANY_HEADERS;
        if (!has_crlf) break;
        count_pos = eol + 2;
    }

    HTTPHeader *extras = NULL;
    if (max_lines){
        extras = (HTTPHeader*)zalloc(sizeof(*extras) * max_lines);
        if (!extras) return HTTP_PARSE_TOO_LARGE;
    }

    HTTPParseResult result = HTTP_PARSE_OK;
    uint32_t extra_i = 0;
    uint32_t pos = 0;
    while (pos < len){
        uint32_t eol = pos;
        bool has_crlf = false;
        while (eol + 1 < len) {
            if (buf[eol] == '\r' && buf[eol+1] == '\n') {
                has_crlf = true;
                break;
            }
            eol++;
        }
        if (!has_crlf) eol = len;

        if (eol == pos) break;

        uint32_t sep = pos;
        while (sep < eol && buf[sep] != ':') sep++;
        if (sep == eol || sep == pos) {
            result = HTTP_PARSE_BAD_FORMAT;
            break;
        }

        uint32_t key_len = sep - pos;
        uint32_t val_start = sep + 1;
        while (val_start < eol && is_whitespace(buf[val_start])) val_start++;
        uint32_t val_end = eol;
        while (val_end > val_start && is_whitespace(buf[val_end - 1])) val_end--;
        uint32_t val_len = val_end - val_start;
        bool handled = false;

        if (key_len == 14 && strncmp_case(buf + pos, "content-length", true, key_len) == 0){
            char len_buf[32];
            bool ok = val_len > 0 && val_len < sizeof(len_buf);
            uint32_t parsed = 0;
            if (ok) {
                memcpy(len_buf, buf + val_start, val_len);
                len_buf[val_len] = 0;
                ok = parse_uint32_dec_exact(len_buf, &parsed);
            }

            if (!ok || (C->framing.has_content_length && C->fields.content_length != parsed)) {
                C->bad_content_length = 1;
                result = HTTP_PARSE_BAD_CONTENT_LENGTH;
            } else {
                C->framing.has_content_length = 1;
                C->fields.content_length = parsed;
            }
            handled = true;
        } else if (key_len == 12 && strncmp_case(buf + pos, "content-type", true, key_len) == 0){
            C->fields.content_type = string_from_literal_length(buf + val_start, val_len);
            handled = true;
        } else if (key_len == 10 && strncmp_case(buf + pos, "connection", true, key_len) == 0){
            C->fields.connection = string_from_literal_length(buf + val_start, val_len);
            C->framing.connection_close = http_header_value_has_token(buf + val_start, val_len, "close", 5) ? 1 : 0;
            C->framing.connection_keep_alive = http_header_value_has_token(buf + val_start, val_len, "keep-alive", 10) ? 1 : 0;
            handled = true;
        } else if (key_len == 4 && strncmp_case(buf + pos, "host", true, key_len) == 0) {
            C->fields.host = string_from_literal_length(buf + val_start, val_len);
            handled = true;
        } else if (key_len == 6 && strncmp_case(buf + pos, "expect", true, key_len) == 0){
            C->fields.expect = string_from_literal_length(buf + val_start, val_len);
            C->framing.expect_continue = http_header_value_has_token(buf + val_start, val_len, "100-continue", 12) ? 1 : 0;
            handled = true;
        } else if (key_len == 5 && strncmp_case(buf + pos, "range", true, key_len) == 0){
            C->fields.range = string_from_literal_length(buf + val_start, val_len);
            C->range.invalid = 1;
            if (val_len > 6 && strncmp_case(buf + val_start, "bytes=", true, 6) == 0) {
                uint32_t rp = val_start + 6;
                uint32_t rend = val_start + val_len;
                uint64_t start = 0;
                uint64_t end = 0;
                bool has_start = false;
                bool has_end = false;
                bool overflow = false;
                while (rp < rend && is_digit(buf[rp])) {
                    has_start = true;
                    uint64_t next = start * 10 + (uint64_t)(buf[rp] - '0');
                    if (next < start) overflow = true;
                    start = next;
                    rp++;
                }
                if (rp < rend && buf[rp] == '-') {
                    rp++;
                    while (rp < rend && is_digit(buf[rp])) {
                        has_end = true;
                        uint64_t next = end * 10 + (uint64_t)(buf[rp] - '0');
                        if (next < end) overflow = true;
                        end = next;
                        rp++;
                    }
                    if (rp == rend && (has_start || has_end) && !(has_start && has_end && end < start) && !overflow) {
                        C->range.has = 1;
                        C->range.invalid = 0;
                        C->range.has_start = has_start ? 1 : 0;
                        C->range.has_end = has_end ? 1 : 0;
                        C->range.start = start;
                        C->range.end = end;
                    }
                }
            }
            handled = true;
        } else if (key_len == 8 && strncmp_case(buf + pos, "location", true, key_len) == 0) {
            C->fields.location = string_from_literal_length(buf + val_start, val_len);
            handled = true;
        } else if (key_len == 13 && strncmp_case(buf + pos, "content-range", true, key_len) == 0) {
            C->fields.content_range = string_from_literal_length(buf + val_start, val_len);
            handled = true;
        } else if (key_len == 17 && strncmp_case(buf + pos, "transfer-encoding", true, key_len) == 0){
            uint32_t te_pos = val_start;
            uint32_t te_end = val_start + val_len;
            uint32_t count = 0;
            bool chunked = false;

            while (te_pos < te_end) {
                while (te_pos < te_end && is_whitespace(buf[te_pos])) te_pos++;
                if (te_pos >= te_end) break;

                uint32_t token_start = te_pos;
                while (te_pos < te_end && buf[te_pos] != ',' && !is_whitespace(buf[te_pos]) && buf[te_pos] != ';') te_pos++;
                uint32_t token_len = te_pos - token_start;
                if (!token_len) {
                    result = HTTP_PARSE_BAD_FORMAT;
                    break;
                }

                while (te_pos < te_end && is_whitespace(buf[te_pos])) te_pos++;
                if (te_pos < te_end && buf[te_pos] == ';') {
                    result = HTTP_PARSE_UNSUPPORTED_TRANSFER;
                    break;
                }
                if (token_len != 7 || strncmp_case(buf + token_start, "chunked", true, 7) != 0) {
                    result = HTTP_PARSE_UNSUPPORTED_TRANSFER;
                    break;
                }

                count++;
                chunked = true;
                if (te_pos < te_end) {
                    if (buf[te_pos] != ',') {
                        result = HTTP_PARSE_BAD_FORMAT;
                        break;
                    }
                    te_pos++;
                }
            }

            if (result == HTTP_PARSE_OK && count != 1) result = HTTP_PARSE_UNSUPPORTED_TRANSFER;
            else if (result == HTTP_PARSE_OK && chunked && !p.allow_chunked) result = HTTP_PARSE_UNSUPPORTED_TRANSFER;
            else if (result == HTTP_PARSE_OK) C->framing.chunked = chunked ? 1 : 0;
            handled = true;
        }

        if (!handled) {
            string key = string_from_literal_length((char*)(buf + pos), key_len);
            string value = string_from_literal_length((char*)(buf + val_start), val_len);

            if (extras && extra_i < max_lines){
                extras[extra_i++] = (HTTPHeader){ key, value };
            } else {
                if (key.mem_length) string_free(key);
                if (value.mem_length) string_free(value);
            }
        }

        if (!has_crlf) break;
        pos = eol + 2;
    }

    if (result == HTTP_PARSE_OK && C->framing.chunked && C->framing.has_content_length) result = HTTP_PARSE_BAD_FORMAT;

    if (result != HTTP_PARSE_OK) {
        http_headers_extra_free(extras, extra_i);
        *out_extra = NULL;
        *out_extra_count = 0;
        return result;
    }

    if (!extras || extra_i == 0){
        if (extras) release(extras);
        *out_extra = NULL;
        *out_extra_count = 0;
        return result;
    }

    if (extra_i == max_lines){
        *out_extra = extras;
        *out_extra_count = extra_i;
        return result;
    }

    HTTPHeader *shr = (HTTPHeader*)zalloc(sizeof(*shr) * extra_i);
    if (shr){
        memcpy(shr, extras, sizeof(*shr) * extra_i);
        release(extras);
        *out_extra = shr;
        *out_extra_count = extra_i;
        return result;
    }

    http_headers_extra_free(extras, extra_i);
    *out_extra = NULL;
    *out_extra_count = 0;
    return HTTP_PARSE_TOO_LARGE;
}

static void http_append_chunked_body(string *out, uintptr_t ptr, uint32_t len) {
    if (!out) return;

    if (len) {
        char hex[16];
        uint32_t n = u64_to_base(hex, len, 16, 0);
        string_append_bytes(out, hex, n);
        string_append_bytes(out, "\r\n", 2);
        string_append_bytes(out, (char*)ptr, len);
        string_append_bytes(out, "\r\n", 2);
    }

    string_append_bytes(out, "0\r\n\r\n", 5);
}

string http_request_builder(const HTTPRequestMsg *R){
    HTTPHeadersCommon common = R->headers_common;
    if (R->host_override) common.fields.host = R->host_override[0] ? string_from_const(R->host_override) : (string){0};
    if (!common.framing.chunked && (R->body.size || R->method == HTTP_METHOD_POST || R->method == HTTP_METHOD_PUT)) {
        common.fields.content_length = (uint32_t)R->body.size;
        common.framing.has_content_length = 1;
    }

    HTTPVersion version = R->version == HTTP_VERSION_10 ? HTTP_VERSION_10 : HTTP_VERSION_11;
    string out = string_format("%s ", http_method_name(R->method));

    string_append_bytes(&out, R->path.data, R->path.length);
    if (version == HTTP_VERSION_10) string_append_bytes(&out, " HTTP/1.0\r\n", 11);
    else string_append_bytes(&out, " HTTP/1.1\r\n", 11);

    string hdrs = http_header_builder(&common, R->extra_headers, R->extra_header_count, HTTP_HEADER_BUILD_REQUEST, R->method, 0);
    string_append_bytes(&out, hdrs.data, hdrs.length);
    string_free(hdrs);

    if (common.framing.chunked) http_append_chunked_body(&out, R->body.ptr, (uint32_t)R->body.size);
    else if (R->body.ptr && R->body.size) string_append_bytes(&out, (char*)R->body.ptr, (uint32_t)R->body.size);

    return out;
}

string http_response_builder(const HTTPResponseMsg *R){
    HTTPHeadersCommon common = R->headers_common;
    bool informational = R->status_code >= 100 && R->status_code < 200;
    if (!informational && !common.framing.chunked) {
        if (!common.framing.has_content_length && !common.fields.content_length) common.fields.content_length = (uint32_t)R->body.size;
        common.framing.has_content_length = 1;
    }

    string out = string_format("HTTP/1.1 %i ", (int)R->status_code);
    if (R->reason.length) string_append_bytes(&out, R->reason.data, R->reason.length);
    else {
        const char *reason = http_status_reason(R->status_code);
        string_append_bytes(&out, reason, (uint32_t)strlen(reason));
    }
    string_append_bytes(&out, "\r\n", 2);

    string hdrs = http_header_builder(&common, R->extra_headers, R->extra_header_count, HTTP_HEADER_BUILD_RESPONSE, HTTP_METHOD_GET, (uint32_t)R->status_code);
    string_append_bytes(&out, hdrs.data, hdrs.length);
    string_free(hdrs);

    if (!informational) {
        if (common.framing.chunked) http_append_chunked_body(&out, R->body.ptr, (uint32_t)R->body.size);
        else if (R->body.ptr && R->body.size) string_append_bytes(&out, (char*)R->body.ptr, (uint32_t)R->body.size);
    }

    return out;
}

int find_crlfcrlf(const char *data, uint32_t len){
    for (uint32_t i = 0; i + 3 < len; i++){
        if (data[i]=='\r' && data[i+1]=='\n' && data[i+2]=='\r' && data[i+3]=='\n') return (int)i;
    }
    return -1;
}

void http_chunked_decoder_init(HTTPChunkedDecoder *dec, const HTTPPolicy *policy){
    if (!dec) return;
    *dec = (HTTPChunkedDecoder){0};
    dec->policy = policy ? *policy : http_default_policy();
    dec->stage = HTTP_CHUNK_STAGE_SIZE;
    dec->line = string_repeat('\0', 0);
    dec->body = string_repeat('\0', 0);
    dec->trailers_buf = string_repeat('\0', 0);
}

HTTPParseResult http_chunked_decoder_feed(HTTPChunkedDecoder *dec, const char *buf, uint32_t len, uint32_t *out_used) {
    if (out_used) *out_used = 0;
    if (!dec || !buf) return HTTP_PARSE_BAD_FORMAT;
    if (dec->stage == HTTP_CHUNK_STAGE_DONE) return HTTP_PARSE_OK; 

    uint32_t i = 0;
    while (i < len) {
        if (dec->stage == HTTP_CHUNK_STAGE_SIZE) {
            string_append_bytes(&dec->line, buf+i, 1);
            i++;
            if (dec->line.length > dec->policy.max_header_value_len) return HTTP_PARSE_TOO_LARGE;
            if (dec->line.length >= 2 && dec->line.data[dec->line.length - 2] == '\r' && dec->line.data[dec->line.length - 1] == '\n') {
                uint64_t chunk_len = 0;
                HTTPParseResult r = HTTP_PARSE_OK;
                uint32_t line_end = dec->line.length - 2;
                uint32_t scan = 0;
                bool saw_digit = false;
                while (scan < line_end && is_whitespace(dec->line.data[scan])) scan++;
                if (scan >= line_end) r = HTTP_PARSE_BAD_FORMAT;
                while (r == HTTP_PARSE_OK && scan < line_end) {
                    char c = dec->line.data[scan];
                    if (c == ';') break;
                    if (is_whitespace(c)) {
                        while (scan < line_end && is_whitespace(dec->line.data[scan])) scan++;
                        if (scan < line_end && dec->line.data[scan] != ';') r = HTTP_PARSE_BAD_FORMAT;
                        break;
                    }

                    int digit = hex_val(c);
                    if (digit < 0) {
                        r = HTTP_PARSE_BAD_FORMAT;
                        break;
                    }
                    saw_digit = true;
                    chunk_len = chunk_len * 16 + (uint64_t)digit;
                    if (chunk_len > dec->policy.max_body_bytes) {
                        r = HTTP_PARSE_PAYLOAD_TOO_LARGE;
                        break;
                    }
                    scan++;
                }
                string_free(dec->line);
                dec->line = string_repeat('\0', 0);
                if (!saw_digit && r == HTTP_PARSE_OK) r = HTTP_PARSE_BAD_FORMAT;
                if (r != HTTP_PARSE_OK) return r;
                if (dec->body_total + chunk_len > dec->policy.max_body_bytes) return HTTP_PARSE_PAYLOAD_TOO_LARGE;
                dec->chunk_size = chunk_len;
                dec->chunk_read = 0;
                dec->crlf_seen = 0;
                dec->stage = chunk_len ? HTTP_CHUNK_STAGE_DATA : HTTP_CHUNK_STAGE_TRAILERS;
            }
        } else if (dec->stage == HTTP_CHUNK_STAGE_DATA) {
            uint64_t need64 = dec->chunk_size - dec->chunk_read;
            uint32_t avail = len - i;
            uint32_t take = need64 < avail ? (uint32_t)need64:  avail; 
            if (take) {
                string_append_bytes(&dec->body, buf + i, take);
                dec->chunk_read += take;
                dec->body_total += take;
                i += take;
            }
            if (dec->chunk_read == dec->chunk_size) dec->stage = HTTP_CHUNK_STAGE_DATA_CRLF;
        } else if (dec->stage == HTTP_CHUNK_STAGE_DATA_CRLF) {
            char expected = dec->crlf_seen ? '\n' : '\r';
            if (buf[i] != expected) return HTTP_PARSE_BAD_FORMAT;
            dec->crlf_seen++;
            i++;
            if (dec->crlf_seen == 2) {
                dec->crlf_seen = 0;
                dec->stage = HTTP_CHUNK_STAGE_SIZE;
            }
        } else if (dec->stage == HTTP_CHUNK_STAGE_TRAILERS) {
            string_append_bytes(&dec->trailers_buf, buf + i, 1);
            i++;
            if (dec->trailers_buf.length > dec->policy.max_header_bytes) return HTTP_PARSE_TOO_LARGE;
            if (dec->trailers_buf.length == 2 && dec->trailers_buf.data[0] == '\r' && dec->trailers_buf.data[1] == '\n') {
                dec->stage = HTTP_CHUNK_STAGE_DONE;
                if (out_used) *out_used = i;
                return HTTP_PARSE_OK;
            }

            if (find_crlfcrlf(dec->trailers_buf.data, dec->trailers_buf.length) >= 0) {
                dec->stage = HTTP_CHUNK_STAGE_DONE;
                if (out_used) *out_used = i;
                return HTTP_PARSE_OK;
            }

        }
    }
    if (out_used) *out_used = i;
    return dec->stage == HTTP_CHUNK_STAGE_DONE ? HTTP_PARSE_OK : HTTP_PARSE_INCOMPLETE;
}

void http_chunked_decoder_free(HTTPChunkedDecoder *dec) {
    if (!dec) return;
    if (dec->line.mem_length) string_free(dec->line);
    if (dec->body.mem_length) string_free(dec->body);
    if (dec->trailers_buf.mem_length) string_free(dec->trailers_buf);
    *dec = (HTTPChunkedDecoder){0};
}
