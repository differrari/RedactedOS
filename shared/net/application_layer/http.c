#include "http.h"
#include "std/string.h"
#include "std/memory.h"
#include "syscalls/syscalls.h"


static inline bool is_space(char c) {
    return c == ' ' || c == '\t';
}

static inline uint32_t parse_u32(const char *s, uint32_t len) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            r = r * 10 + (uint32_t)(c - '0');
        } else {
            break;
        }
    }
    return r;
}

string http_header_builder(const HTTPHeadersCommon *C,
                           const HTTPHeader *H, uint32_t N)
{
    string out = string_repeat('\0', 0);

    if (C->type.length) {
        string_append_bytes(&out, "Content-Type: ", 14);
        string_append_bytes(&out,
                            C->type.data,
                            C->type.length);
        string_append_bytes(&out, "\r\n", 2);
    }

    if (C->length) {
        string tmp = string_format("Content-Length: %i\r\n",
                                (int)C->length);
        string_append_bytes(&out, tmp.data, tmp.length);
        free(tmp.data, tmp.mem_length);
    }

    if (C->date.length) {
        string_append_bytes(&out, "Date: ", 6);
        string_append_bytes(&out,
                            C->date.data,
                            C->date.length);
        string_append_bytes(&out, "\r\n", 2);
    }

    if (C->host.length) {
        string_append_bytes(&out, "Host: ", 6);
        bool has_colon = str_has_char(C->host.data, C->host.length, ':');
        bool has_lb = str_has_char(C->host.data, C->host.length, '[');
        bool has_rb = str_has_char(C->host.data, C->host.length, ']');
        if (has_colon && !has_lb && !has_rb) {
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

    if (C->connection.length) {
        string_append_bytes(&out, "Connection: ", 12);
        string_append_bytes(&out,
                            C->connection.data,
                            C->connection.length);
        string_append_bytes(&out, "\r\n", 2);
    }

    if (C->keep_alive.length) {
        string_append_bytes(&out, "Keep-Alive: ", 12);
        string_append_bytes(&out,
                            C->keep_alive.data,
                            C->keep_alive.length);
        string_append_bytes(&out, "\r\n", 2);
    }

    for (uint32_t i = 0; i < N; i++) {
        const HTTPHeader *hdr = &H[i];
        string_append_bytes(&out,
                            hdr->key.data,
                            hdr->key.length);
        string_append_bytes(&out, ": ", 2);
        string_append_bytes(&out,
                            hdr->value.data,
                            hdr->value.length);
        string_append_bytes(&out, "\r\n", 2);
    }

    string_append_bytes(&out, "\r\n", 2);
    return out;
}


void http_headers_common_free(HTTPHeadersCommon *C) {
    if (!C) return;
    if (C->type.mem_length) free(C->type.data, C->type.mem_length);
    if (C->date.mem_length) free(C->date.data, C->date.mem_length);
    if (C->connection.mem_length) free(C->connection.data, C->connection.mem_length);
    if (C->keep_alive.mem_length) free(C->keep_alive.data, C->keep_alive.mem_length);
    if (C->host.mem_length) free(C->host.data, C->host.mem_length);
    if (C->content_type.mem_length) free(C->content_type.data, C->content_type.mem_length);
    *C = (HTTPHeadersCommon){0};
}

void http_headers_extra_free(HTTPHeader *extra, uint32_t extra_count) {
    if (!extra) return;
    for (uint32_t i = 0; i < extra_count; i++) {
        if (extra[i].key.mem_length)free(extra[i].key.data, extra[i].key.mem_length);
        if (extra[i].value.mem_length)free(extra[i].value.data, extra[i].value.mem_length);
    }
    free(extra, extra_count * sizeof(HTTPHeader));
}

void http_header_parser(const char *buf, uint32_t len,
                        HTTPHeadersCommon *C,
                        HTTPHeader **out_extra,
                        uint32_t *out_extra_count)
{
    *C = (HTTPHeadersCommon){0};

    uint32_t max_lines = 0;
    for (uint32_t i = 0; i + 1 < len; i++) {
        if (buf[i]=='\r' && buf[i+1]=='\n') 
            max_lines++;
    }

    HTTPHeader *extras = NULL;
    if (max_lines) {
        extras = (HTTPHeader*)(uintptr_t)malloc(sizeof(*extras) * max_lines);
        if (!extras) {
            *out_extra = NULL;
            *out_extra_count = 0;
            return;
        }
    }

    uint32_t extra_i = 0;
    uint32_t pos = 0;

    char key_tmp[64];

    while (pos + 1 < len) {
        uint32_t eol = pos;
        while (eol + 1 < len && !(buf[eol]=='\r' && buf[eol+1]=='\n'))
            eol++;
        if (eol == pos) {
            pos += 2;
            break;
        }

        uint32_t sep = pos;
        while (sep < eol && buf[sep] != ':') sep++;
        uint32_t key_len = sep - pos;
        uint32_t val_start = sep + 1;
        while (val_start < eol && is_space((unsigned char)buf[val_start]))
            val_start++;
        uint32_t val_len = eol - val_start;

        uint32_t copy_len = (key_len < sizeof(key_tmp)-1) ? key_len : (sizeof(key_tmp)-1);
        for (uint32_t i = 0; i < copy_len; i++) {
            key_tmp[i] = buf[pos + i];
        }
        key_tmp[copy_len] = '\0';

        if (copy_len == 14 && strcmp(key_tmp, "content-length", true) == 0) {
            C->length = parse_u32(buf + val_start, val_len);
        }
        else if (copy_len == 12 && strcmp(key_tmp, "content-type", true) == 0) {
            if (C->type.data && C->type.mem_length) free(C->type.data, C->type.mem_length);
            C->type = (string){0};
            C->type = string_from_literal_length((char*)(buf + val_start), val_len);
        }
        else if (copy_len == 4 && strcmp(key_tmp, "date", true) == 0) {
            if (C->date.data && C->date.mem_length) free(C->date.data, C->date.mem_length);
            C->date = (string){0};
            C->date = string_from_literal_length((char*)(buf + val_start), val_len);
        }
        else if (copy_len == 10 && strcmp(key_tmp, "connection", true) == 0) {
            if (C->connection.data && C->connection.mem_length) free(C->connection.data, C->connection.mem_length);
            C->connection = (string){0};
            C->connection = string_from_literal_length((char*)(buf + val_start), val_len);
        }
        else if (copy_len == 10 && strcmp(key_tmp, "keep-alive", true) == 0) {
            if (C->keep_alive.data && C->keep_alive.mem_length) free(C->keep_alive.data, C->keep_alive.mem_length);
            C->keep_alive = (string){0};
            C->keep_alive = string_from_literal_length((char*)(buf + val_start), val_len);
        }
        else if (copy_len == 4 && strcmp(key_tmp, "host", true) == 0) {
            if (C->host.data && C->host.mem_length) free(C->host.data, C->host.mem_length);
            C->host = (string){0};
            C->host = string_from_literal_length((char*)(buf + val_start), val_len);
        }
        else {
            string key = string_from_literal_length((char*)(buf + pos), key_len);
            string value = string_from_literal_length((char*)(buf + val_start), val_len);
            if (extras && extra_i< max_lines) extras[extra_i++] = (HTTPHeader){ key, value };
            else {
                if (key.mem_length) free(key.data, key.mem_length);
                if (value.mem_length) free(value.data, value.mem_length);
            }
        }

        pos = eol + 2;
    }

    if (!extras || extra_i == 0) {
        if (extras) free(extras, sizeof(*extras) * max_lines);
        *out_extra = NULL;
        *out_extra_count = 0;
        return;
    }

    if (extra_i == max_lines){
        *out_extra = extras;
        *out_extra_count = extra_i;
        return;
    }

    HTTPHeader *shr = (HTTPHeader*)(uintptr_t)malloc(sizeof(*shr) *extra_i);
    if (shr) {
        memcpy(shr, extras, sizeof(*shr) * extra_i);
        free(extras,sizeof(*extras) * max_lines);
        *out_extra = shr;
        *out_extra_count = extra_i;
        return;
    }

    for (uint32_t i = 0; i < extra_i; i++) {
        if (extras[i].key.mem_length) free(extras[i].key.data, extras[i].key.mem_length);
        if (extras[i].value.mem_length) free(extras[i].value.data, extras[i].value.mem_length);
    }
    free(extras, sizeof(*extras) * max_lines);
    *out_extra = NULL;
    *out_extra_count = 0;
}

string http_request_builder(const HTTPRequestMsg *R)
{
    static const char *Mnames[] = { "GET", "POST", "PUT", "DELETE" };
    string out = string_format("%s ", Mnames[R->method]);

    string_append_bytes(&out, R->path.data, R->path.length);

    string_append_bytes(&out, " HTTP/1.1\r\n", 11);

    string hdrs = http_header_builder(
        &R->headers_common,
        R->extra_headers, R->extra_header_count);
    string_append_bytes(&out, hdrs.data, hdrs.length);
    free(hdrs.data, hdrs.mem_length);

    if (R->body.ptr && R->body.size) {
        string body = string_from_literal_length((char*)R->body.ptr, R->body.size);
        string_append_bytes(&out, body.data, body.length);
        free(body.data, body.mem_length);
    }

    return out;
}

string http_response_builder(const HTTPResponseMsg *R) {
    string out = string_format("HTTP/1.1 %i ", (int)R->status_code);
    string_append_bytes(&out,
                        R->reason.data,
                        R->reason.length);
    string_append_bytes(&out, "\r\n", 2);

    string hdrs = http_header_builder(
        &R->headers_common,
        R->extra_headers,
        R->extra_header_count
    );
    string_append_bytes(&out, hdrs.data, hdrs.length);
    free(hdrs.data, hdrs.mem_length);

    if (R->body.ptr && R->body.size) {
        string_append_bytes(&out,
                            (char*)R->body.ptr,
                            (uint32_t)R->body.size);
    }
    return out;
}


int find_crlfcrlf(const char *data, uint32_t len) {
    for (uint32_t i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' &&
            data[i+1] == '\n' &&
            data[i+2] == '\r' &&
            data[i+3] == '\n')
        {
            return (int)i;
        }
    }
    return -1;
}

sizedptr http_get_payload(sizedptr header) {
    if (!header.ptr || header.size < 4) {
        return (sizedptr){0};
    }
    int start = find_crlfcrlf((char*)header.ptr, header.size);
    if (start < 0) {
        return (sizedptr){0};
    }
    return (sizedptr){
        header.ptr + (uint32_t)(start + 4),
        header.size - (uint32_t)(start + 4)
    };
}

string http_get_chunked_payload(sizedptr chunk) {
    if (chunk.ptr && chunk.size > 0) {
        int sizetrm = strindex((char*)chunk.ptr, "\r\n");
        uint64_t chunk_size = parse_hex_u64((char*)chunk.ptr, sizetrm);
        return string_from_literal_length((char*)(chunk.ptr + sizetrm + 2),
                            (uint32_t)chunk_size);
    }
    return (string){0};
}