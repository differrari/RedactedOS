#pragma once

#include "types.h"
#include "net/net_ctrl.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t net_ctrl_dispatch(const void* req, uint32_t req_len, uint8_t** out, uint32_t* out_len);

#ifdef __cplusplus
}
#endif