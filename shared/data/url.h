#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    sizedptr scheme;
    sizedptr userinfo;
    sizedptr host;
    uint16_t port;
    sizedptr path;
    sizedptr query;
    sizedptr fragment;
    bool ok;
} ParsedURL;

ParsedURL parse_url(const char *buf, uint32_t len);
ParsedURL parse_url_z(const char *buf);

#ifdef __cplusplus
}
#endif
