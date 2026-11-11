#include "ssdp.h"
#include "networking/application_layer/http.h"
#include "std/string.h"
#include "std/memory.h"

uint32_t ssdp_parse_mx_ms(const char* buf, int len){
    if (!buf || len <= 0) return 1000;
    int i = 0;
    for (; i + 4 <= len; i++) {
        if (buf[i] == '\n' && strncmp_case(buf + i + 1, "mx:", 1, 3) == 0) break;
    }
    if (i + 4 > len) return 1000;
    i += 4;

    while (i < len && (buf[i] == ' ' || buf[i] == '\t')) ++i;
    uint32_t start = (uint32_t)i;
    while (i < len && is_digit(buf[i])) ++i;
    if ((uint32_t)i == start) return 1000;

    uint32_t mx_len = i - start;
    uint32_t v = 0;
    if (mx_len >= 16 || !parse_uint32_dec_len(buf + start, mx_len, &v)) return 1000;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t')) i++;
    if (i < len && buf[i] != '\r' && buf[i] != '\n') return 1000;
    if (i < len && buf[i] == '\r' && (i + 1 == len || buf[i + 1] != '\n')) return 1000;

    if (v == 0) v = 1;
    if (v > 5) v = 5;
    return v * 1000;
}

bool ssdp_is_msearch(const char* buf, int len) {
    if (!buf || len < 8) return false;
    if (memcmp(buf, "M-SEARCH", 8) != 0) return false;
    if (!memmem(buf, len, "ssdp:discover", 13)) return false;
    return true;
}

string ssdp_build_search_response(void) {
    HTTPHeader extra[] = {
        {{"CACHE-CONTROL", 13, 0}, {"max-age=60", 10, 0}},
        {{"EXT", 3, 0}, {"", 0, 0}},
        {{"SERVER", 6, 0}, {"RedactedOS/1.0 UPnP/1.1", 23, 0}},
        {{"ST", 2, 0}, {"ssdp:all", 8, 0}},
        {{"USN", 3, 0}, {"uuid:redacted-os::upnp:rootdevice", 33, 0}}
    };

    HTTPResponseMsg R = (HTTPResponseMsg){0};
    R.status_code = HTTP_OK;
    R.reason = (string){"OK", 2, 0};
    R.extra_headers = extra;
    R.extra_header_count = N_ARR(extra);
    return http_response_builder(&R);
}

string ssdp_build_notify(bool alive, bool v6) {
    const char* host = v6 ? "[ff02::c]:1900" : "239.255.255.250:1900";
    const char* nts = alive ? "ssdp:alive" : "ssdp:byebye";

    string out = string_from_literal("NOTIFY * HTTP/1.1\r\n");
    if (!out.data) return out;
    HTTPHeader extra[] = {
        {{"HOST", 4, 0}, {(char*)host, (uint32_t)strlen(host), 0}},
        {{"NT", 2, 0}, {"upnp:rootdevice", 15, 0}},
        {{"NTS", 3, 0}, {(char*)nts, (uint32_t)strlen(nts), 0}},
        {{"USN", 3, 0}, {"uuid:redacted-os::upnp:rootdevice", 33, 0}},
        {{"CACHE-CONTROL", 13, 0}, {"max-age=1800", 12, 0}},
        {{"SERVER", 6, 0}, {"RedactedOS/1.0 UPnP/1.1", 23, 0}}
    };

    HTTPHeadersCommon c = (HTTPHeadersCommon){0};
    string hdrs = http_header_builder(&c, extra, N_ARR(extra), HTTP_HEADER_BUILD_REQUEST, HTTP_METHOD_GET, 0);
    if (!hdrs.data || hdrs.length > UINT32_MAX - out.length) {
        string_free(out);
        string_free(hdrs);
        return (string){0};
    }
    uint32_t total = out.length + hdrs.length;
    string_append_bytes(&out, hdrs.data, hdrs.length);
    string_free(hdrs);
    if (out.length != total) {
        string_free(out);
        return (string){0};
    }
    return out;
}
