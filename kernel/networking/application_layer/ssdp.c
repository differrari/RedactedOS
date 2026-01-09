#include "ssdp.h"
#include "syscalls/syscalls.h"
#include "networking/application_layer/http.h"
#include "std/std.h"

uint32_t ssdp_parse_mx_ms(const char* buf, int len){
    const char* p = memmem(buf, len, "\r\nmx:", 5);
    if (!p) p = memmem(buf, len, "\nmx:", 4);
    if (!p) return 1000;

    int i = (int)(p - buf);
    while (i < len && buf[i] != ':') ++i;
    if (i >= len) return 1000;
    ++i;

    while (i < len && (buf[i] == ' ' || buf[i] == '\t')) ++i;

    uint32_t v = 0;
    parse_uint32_dec(buf, &v);

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
    HTTPHeader extra[5];
    extra[0] = (HTTPHeader){ string_from_literal("CACHE-CONTROL"), string_from_literal("max-age=60")};
    extra[1] = (HTTPHeader){ string_from_literal("EXT"), string_from_literal("")};
    extra[2] = (HTTPHeader){ string_from_literal("SERVER"), string_from_literal("RedactedOS/1.0 UPnP/1.1")};
    extra[3] = (HTTPHeader){ string_from_literal("ST"), string_from_literal("ssdp:all")};
    extra[4] = (HTTPHeader){ string_from_literal("USN"), string_from_literal("uuid:redacted-os::upnp:rootdevice")};

    HTTPResponseMsg R = (HTTPResponseMsg){0};
    R.status_code = HTTP_OK;
    R.reason = string_from_literal("OK");
    R.extra_headers = extra;
    R.extra_header_count = 5;
    return http_response_builder(&R);
}

string ssdp_build_notify(bool alive, bool v6) {
    const char* host = v6 ? "[ff02::c]:1900" : "239.255.255.250:1900";
    const char* nts = alive ? "ssdp:alive" : "ssdp:byebye";

    string out = string_from_literal("NOTIFY * HTTP/1.1\r\n");

    HTTPHeader extra[6];
    extra[0] = (HTTPHeader){ string_from_literal("HOST"), string_from_literal(host)};
    extra[1] = (HTTPHeader){ string_from_literal("NT"), string_from_literal("upnp:rootdevice")};
    extra[2] = (HTTPHeader){ string_from_literal("NTS"), string_from_literal(nts)};
    extra[3] = (HTTPHeader){ string_from_literal("USN"), string_from_literal("uuid:redacted-os::upnp:rootdevice")};
    extra[4] = (HTTPHeader){ string_from_literal("CACHE-CONTROL"), string_from_literal("max-age=1800")};
    extra[5] = (HTTPHeader){ string_from_literal("SERVER"), string_from_literal("RedactedOS/1.0 UPnP/1.1")};

    HTTPHeadersCommon c = (HTTPHeadersCommon){0};
    string hdrs = http_header_builder(&c, extra, 6);
    string_append_bytes(&out, hdrs.data, hdrs.length);
    free_sized(hdrs.data, hdrs.mem_length);
    string_append_bytes(&out, "\r\n", 2);
    return out;
}
