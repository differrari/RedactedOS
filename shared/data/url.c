#include "url.h"
#include "std/string.h"

ParsedURL parse_url(const char *buf, uint32_t len) {
    ParsedURL r = {0};
    if (!buf || !len) return r;

    uint32_t i = 0;

    if (!is_alpha(buf[i]))return r;
    i++;
    while (i < len) {
        char c= buf[i];
        if (c == ':') break;

        if (!(is_alnum(c) || c == '+' || c == '-' || c == '.')) return r;
        i++;
    }

    if (i >= len || buf[i] != ':') return r;

    r.scheme.ptr = (uintptr_t)buf;
    r.scheme.size = i;
    i++;

    if (i + 1 < len && buf[i] =='/' &&buf[i + 1] == '/') {
        i += 2;
        uint32_t auth_start = i;
        uint32_t auth_end = auth_start;
        while (auth_end < len) {
            char c = buf[auth_end];
            if (c == '/' || c == '?' || c == '#') break;
            auth_end++;
        }

        if (auth_end > auth_start) {
            uint32_t user_end = auth_start;
            while (user_end<auth_end && buf[user_end] != '@') user_end++;

            uint32_t host_start;
            if (user_end < auth_end && buf[user_end] == '@') {
                r.userinfo.ptr = (uintptr_t)(buf + auth_start);
                r.userinfo.size = user_end - auth_start;
                host_start = user_end + 1;
            } else host_start = auth_start;

            if (host_start >= auth_end) return r;

            if (buf[host_start] == '[') {
                uint32_t p = host_start + 1;
                while (p < auth_end && buf[p] != ']') p++;

                if (p >= auth_end) return r;

                r.host.ptr = (uintptr_t)(buf + host_start + 1);
                r.host.size = p -(host_start + 1);

                p++;
                if (p < auth_end && buf[p] == ':') {
                    p++;
                    uint32_t v = 0;
                    int any = 0;
                    while (p < auth_end) {
                        char d = buf[p];
                        if (!is_digit(d)) return r;

                        any = 1;
                        v = v * 10 + (uint32_t)(d - '0');
                        if (v > 65535) return r;
                        p++;
                    }
                    if (!any) return r;
                    r.port = (uint16_t)v;
                }
            } else {
                uint32_t p = host_start;
                while (p < auth_end && buf[p] != ':') p++;

                r.host.ptr = (uintptr_t)(buf + host_start);
                r.host.size = p -host_start;

                if (p < auth_end && buf[p] == ':') {
                    p++;
                    uint32_t port = 0;
                    int any = 0;
                    while (p < auth_end) {
                        char d = buf[p];
                        if (!is_digit(d)) return r;

                        any = 1;
                        port = port * 10 + (uint32_t)(d -'0');
                        if (port > 65535) return r;
                        p++;
                    }
                    if (!any) return r;
                    r.port = (uint16_t)port;
                }
            }
        }

        i = auth_end;
    }

    uint32_t path_start = i;
    uint32_t path_end = path_start;
    while (path_end < len) {
        char c = buf[path_end];
        if (c == '?' || c == '#') break;
        path_end++;
    }

    if (path_end > path_start) {
        r.path.ptr = (uintptr_t)(buf + path_start);
        r.path.size = path_end - path_start;
    }

    i = path_end;

    if (i < len && buf[i] == '?') {
        uint32_t qs = i + 1;
        uint32_t qe = qs;
        while (qe < len && buf[qe] != '#') qe++;
        
        if (qe > qs) {
            r.query.ptr = (uintptr_t)(buf + qs);
            r.query.size = qe - qs;
        }
        i = qe;
    }

    if (i < len && buf[i] == '#') {
        uint32_t fs = i + 1;
        if (fs < len) {
            r.fragment.ptr = (uintptr_t)(buf + fs);
            r.fragment.size = len - fs;
        }
    }

    r.ok = true;
    return r;
}

ParsedURL parse_url_z(const char *buf) {
    if (!buf) {
        ParsedURL r = {0};
        return r;
    }
    size_t n = strlen(buf);
    return parse_url(buf, (uint32_t)n);
}
