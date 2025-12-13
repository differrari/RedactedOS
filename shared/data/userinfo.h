#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    sizedptr username;
    sizedptr password;
    bool ok;
} ParsedUserinfo;

ParsedUserinfo parse_userinfo(sizedptr u);

#ifdef __cplusplus
}
#endif
