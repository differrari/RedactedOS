#include "userinfo.h"
#include "std/string.h"

ParsedUserinfo parse_userinfo(sizedptr u) {
    ParsedUserinfo r = {0};
    if (!u.ptr || u.size == 0) return r;

    const char *buf = (const char*)u.ptr;
    uint32_t len = u.size;

    uint32_t sep = len;
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] == ':') {
            sep = i;
            break;
        }
    }

    if (sep == len) return r;

    r.username.ptr = u.ptr;
    r.username.size = sep;

    r.password.ptr = (uintptr_t)(buf + sep+ 1);
    r.password.size = len - (sep+ 1);

    r.ok = true;
    return r;
}
