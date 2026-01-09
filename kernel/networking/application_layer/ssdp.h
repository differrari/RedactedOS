#pragma once

#include "types.h"
#include "std/string.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IPV4_MCAST_SSDP 0xEFFFFFFAu

bool ssdp_is_msearch(const char* buf, int len);
uint32_t ssdp_parse_mx_ms(const char* buf, int len);

string ssdp_build_search_response(void);
string ssdp_build_notify(bool alive, bool v6);

#ifdef __cplusplus
}
#endif