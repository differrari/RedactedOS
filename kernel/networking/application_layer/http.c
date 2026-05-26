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
        .max_keepalive_requests = HTTP_DEFAULT_MAX_KEEPALIVE_REQUESTS,
        .max_redirects = HTTP_DEFAULT_MAX_REDIRECTS,
        .allow_chunked = true,
        .allow_keep_alive = false,
        .allow_absolute_uri = true,
        .require_host_http11 = true
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
    if (options->max_keepalive_requests > 0) p.max_keepalive_requests = options->max_keepalive_requests;
    if (options->max_redirects > 0) p.max_redirects = options->max_redirects;

    p.allow_chunked = options->allow_chunked != 0;
    p.allow_keep_alive = options->allow_keep_alive != 0;
    p.allow_absolute_uri = options->allow_absolute_uri != 0;
    p.require_host_http11 = options->require_host_http11 != 0;
    return p;
}

const char* http_status_reason(HttpError status) {
    switch (status) {
        case HTTP_OK: return "OK";
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
    if (!parse_uint32_dec(code_buf, &code)) return HTTP_PARSE_BAD_FORMAT;
    out->status_code = code;

    uint32_t i = 12;
    while (i < len && buf[i] == ' ') i++;
    out->reason_off = i;
    out->reason_len = len - i;
    return HTTP_PARSE_OK;
}

string http_header_builder(const HTTPHeadersCommon *C, const HTTPHeader *H, uint32_t N){
    string out = string_repeat('\0', 0);

    if (C->type.length){
        string_append_bytes(&out, "Content-Type: ", 14);
        string_append_bytes(&out, C->type.data, C->type.length);
        string_append_bytes(&out, "\r\n", 2);
    }

    if (C->chunked) string_append_bytes(&out, "Transfer-Encoding: chunked\r\n", 28);
    else {
        string tmp = string_format("Content-Length: %i\r\n", (int)C->length);
        string_append_bytes(&out, tmp.data, tmp.length);
        string_free(tmp);
    }

    if (C->date.length){
        string_append_bytes(&out, "Date: ", 6);
        string_append_bytes(&out, C->date.data, C->date.length);
        string_append_bytes(&out, "\r\n", 2);
    }

    if (C->host.length){
        string_append_bytes(&out, "Host: ", 6);
        bool has_colon = str_has_char(C->host.data, C->host.length, ':') >= 0;
        bool has_lb = str_has_char(C->host.data, C->host.length, '[') >= 0;
        bool has_rb = str_has_char(C->host.data, C->host.length, ']') >= 0;
        if (has_colon && !has_lb && !has_rb){
            string_append_bytes(&out, "[", 1);
            string_append_bytes(&out, C->host.data, C->host.length);
            string_append_bytes(&out, "]", 1);
        } else {
            string_append_bytes(&out, C->host.data, C->host.length);
        }
        string_append_bytes(&out, "\r\n", 2);
    } else {
        string_append_bytes(&out, "Host: RedactedOS_0.1\r\n", 22);
    }

    if (C->connection.length){
        string_append_bytes(&out, "Connection: ", 12);
        string_append_bytes(&out, C->connection.data, C->connection.length);
        string_append_bytes(&out, "\r\n", 2);
    }

    if (C->keep_alive.length){
        string_append_bytes(&out, "Keep-Alive: ", 12);
        string_append_bytes(&out, C->keep_alive.data, C->keep_alive.length);
        string_append_bytes(&out, "\r\n", 2);
    }

    for (uint32_t i = 0; i < N; i++){
        const HTTPHeader *hdr = &H[i];
        string_append_bytes(&out, hdr->key.data, hdr->key.length);
        string_append_bytes(&out, ": ", 2);
        string_append_bytes(&out, hdr->value.data, hdr->value.length);
        string_append_bytes(&out, "\r\n", 2);
    }

    string_append_bytes(&out, "\r\n", 2);
    return out;
}

void http_headers_common_free(HTTPHeadersCommon *C){
    if (!C) return;
    if (C->type.mem_length) string_free(C->type);
    if (C->date.mem_length) string_free(C->date);
    if (C->connection.mem_length) string_free(C->connection);
    if (C->keep_alive.mem_length) string_free(C->keep_alive);
    if (C->host.mem_length) string_free(C->host);
    if (C->content_type.mem_length) string_free(C->content_type);
    if (C->transfer_encoding.mem_length) string_free(C->transfer_encoding);
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

HTTPParseResult http_header_parse_policy(const char *buf, uint32_t len,
                                      const HTTPPolicy *policy,
                                      HTTPHeadersCommon *C,
                                      HTTPHeader **out_extra,
                                      uint32_t *out_extra_count)
{
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

        uint32_t key_len = sep - pos;
        uint32_t val_start = sep + 1;
        while (val_start < eol && is_whitespace(buf[val_start])) val_start++;
        uint32_t val_end = eol;
        while (val_end > val_start && is_whitespace(buf[val_end - 1])) val_end--;
        uint32_t val_len = val_end - val_start;

        if (key_len == 14 && strncmp_case(buf + pos, "content-length", true, key_len) == 0){
            char len_buf[32];
            bool ok = val_len > 0 && val_len < sizeof(len_buf);
            uint32_t parsed = 0;
            if (ok) {
                memcpy(len_buf, buf + val_start, val_len);
                len_buf[val_len] = 0;
                ok = parse_uint32_dec(len_buf, &parsed);
            }

            if (!ok || (C->has_length && C->length != parsed)) {
                C->bad_length = 1;
                result = HTTP_PARSE_BAD_CONTENT_LENGTH;
            } else {
                C->has_length = 1;
                C->length = parsed;
            }
        }
        else if (key_len == 12 && strncmp_case(buf + pos, "content-type", true, key_len) == 0){
            C->type = string_from_literal_length(buf + val_start, val_len);
            C->content_type = string_from_literal_length(buf + val_start, val_len);
        }
        else if (key_len == 4 && strncmp_case(buf + pos, "date", true, key_len) == 0) C->date = string_from_literal_length(buf + val_start, val_len);
        else if (key_len == 10 && strncmp_case(buf + pos, "connection", true, key_len) == 0){
            C->connection = string_from_literal_length(buf + val_start, val_len);
            C->connection_close = http_header_value_has_token(buf + val_start, val_len, "close", 5) ? 1 : 0;
            C->connection_keep_alive = http_header_value_has_token(buf + val_start, val_len, "keep-alive", 10) ? 1 : 0;
        }
        else if (key_len == 10 && strncmp_case(buf + pos, "keep-alive", true, key_len) == 0) C->keep_alive = string_from_literal_length(buf + val_start, val_len);
        else if (key_len == 4 && strncmp_case(buf + pos, "host", true, key_len) == 0) C->host = string_from_literal_length(buf + val_start, val_len);
        else if (key_len == 17 && strncmp_case(buf + pos, "transfer-encoding", true, key_len) == 0){
            C->has_transfer_encoding = 1;
            C->transfer_encoding = string_from_literal_length(buf + val_start, val_len);
            C->chunked = http_header_value_has_token(buf + val_start, val_len, "chunked", 7) ? 1 : 0;
            if (C->chunked && !p.allow_chunked) {
                C->bad_transfer = 1;
                result = HTTP_PARSE_UNSUPPORTED_TRANSFER;
            }
        }
        else {
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

    for (uint32_t i = 0; i < extra_i; i++){
        if (extras[i].key.mem_length) string_free(extras[i].key);
        if (extras[i].value.mem_length) string_free(extras[i].value);
    }
    release(extras);
    *out_extra = NULL;
    *out_extra_count = 0;
    return HTTP_PARSE_TOO_LARGE;
}

static void http_append_chunked_body(string *out, uintptr_t ptr, uint32_t len) {
    if (!out) return;

    if (len) {
        string tmp = string_format("%x\r\n", (int)len);
        string_append_bytes(out, tmp.data, tmp.length);
        string_free(tmp);
        string_append_bytes(out, (char*)ptr, len);
        string_append_bytes(out, "\r\n", 2);
    }

    string_append_bytes(out, "0\r\n\r\n", 5);
}

string http_request_builder(const HTTPRequestMsg *R){
    static const char *Mnames[] = { "GET", "POST", "PUT", "DELETE" };
    string out = string_format("%s ", Mnames[R->method]);

    string_append_bytes(&out, R->path.data, R->path.length);
    string_append_bytes(&out, " HTTP/1.1\r\n", 11);

    string hdrs = http_header_builder(&R->headers_common, R->extra_headers, R->extra_header_count);
    string_append_bytes(&out, hdrs.data, hdrs.length);
    string_free(hdrs);

    if (R->headers_common.chunked) http_append_chunked_body(&out, R->body.ptr, (uint32_t)R->body.size);
    else if (R->body.ptr && R->body.size) string_append_bytes(&out, (char*)R->body.ptr, (uint32_t)R->body.size);

    return out;
}

string http_response_builder(const HTTPResponseMsg *R){
    string out = string_format("HTTP/1.1 %i ", (int)R->status_code);
    if (R->reason.length) string_append_bytes(&out, R->reason.data, R->reason.length);
    else {
        const char *reason = http_status_reason(R->status_code);
        string_append_bytes(&out, reason, (uint32_t)strlen(reason));
    }
    string_append_bytes(&out, "\r\n", 2);

    string hdrs = http_header_builder(&R->headers_common, R->extra_headers, R->extra_header_count);
    string_append_bytes(&out, hdrs.data, hdrs.length);
    string_free(hdrs);

    if (R->headers_common.chunked) http_append_chunked_body(&out, R->body.ptr, (uint32_t)R->body.size);
    else if (R->body.ptr && R->body.size) string_append_bytes(&out, (char*)R->body.ptr, (uint32_t)R->body.size);

    return out;
}

int find_crlfcrlf(const char *data, uint32_t len){
    for (uint32_t i = 0; i + 3 < len; i++){
        if (data[i]=='\r' && data[i+1]=='\n' && data[i+2]=='\r' && data[i+3]=='\n') return (int)i;
    }
    return -1;
}

sizedptr http_get_payload(sizedptr header){
    if (!header.ptr || header.size < 4) return (sizedptr){0};

    int start = find_crlfcrlf((char*)header.ptr, header.size);
    if (start < 0) return (sizedptr){0};

    return (sizedptr){
        header.ptr + (uint32_t)(start + 4),
        header.size - (uint32_t)(start + 4)
    };
}

HTTPParseResult http_decode_chunked_body(const char *buf, uint32_t len, const HTTPPolicy *policy, string *out_body, uint32_t *out_used) {
    HTTPPolicy p = policy ? *policy : http_default_policy();
    if (!buf || !out_body || !out_used) return HTTP_PARSE_BAD_FORMAT;

    string out = string_repeat('\0', 0);
    uint32_t off = 0;
    uint32_t total = 0;

    while (1) {
        uint32_t line_end = off;
        while (line_end + 1 < len && !(buf[line_end] == '\r' && buf[line_end+1] == '\n')) line_end++;
        if (line_end + 1 >= len) {
            string_free(out);
            return HTTP_PARSE_INCOMPLETE;
        }

        uint32_t scan = off;
        while (scan < line_end && is_whitespace(buf[scan])) scan++;
        if (scan >= line_end) {
            string_free(out);
            return HTTP_PARSE_BAD_FORMAT;
        }

        uint64_t chunk_len = 0;
        bool saw_digit = false;
        while (scan < line_end) {
            char c = buf[scan];
            if (c == ';') break;
            if (is_whitespace(c)) {
                while (scan < line_end && is_whitespace(buf[scan])) scan++;
                if (scan < line_end && buf[scan] != ';') {
                    string_free(out);
                    return HTTP_PARSE_BAD_FORMAT;
                }
                break;
            }

            int digit = hex_val(c);
            if (digit < 0) {
                string_free(out);
                return HTTP_PARSE_BAD_FORMAT;
            }
            saw_digit = true;
            chunk_len = chunk_len * 16 + digit;
            if (chunk_len > p.max_body_bytes || total + chunk_len > p.max_body_bytes) {
                string_free(out);
                return HTTP_PARSE_PAYLOAD_TOO_LARGE;
            }
            scan++;
        }

        if (!saw_digit) {
            string_free(out);
            return HTTP_PARSE_BAD_FORMAT;
        }

        off = line_end + 2;
        if (chunk_len == 0) {
            if (off + 1 < len && buf[off] == '\r' && buf[off+1] == '\n') {
                *out_body = out;
                *out_used = off + 2;
                return HTTP_PARSE_OK;
            }

            int trailers_end = find_crlfcrlf(buf + off, len - off);
            if (trailers_end >= 0) {
                *out_body = out;
                *out_used = off + (uint32_t)trailers_end + 4;
                return HTTP_PARSE_OK;
            }

            string_free(out);
            return HTTP_PARSE_INCOMPLETE;
        }

        if (off + chunk_len + 2 > len) {
            string_free(out);
            return HTTP_PARSE_INCOMPLETE;
        }
        if (buf[off + chunk_len] != '\r' || buf[off+1 + chunk_len] != '\n') {
            string_free(out);
            return HTTP_PARSE_BAD_FORMAT;
        }

        string_append_bytes(&out, buf + off, (uint32_t)chunk_len);
        total += (uint32_t)chunk_len;
        off += (uint32_t)chunk_len + 2;
    }
}
