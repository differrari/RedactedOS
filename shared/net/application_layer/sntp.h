#pragma once//deprecated, use ntp
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SNTP_OK = 0,
    SNTP_ERR_NO_CFG,
    SNTP_ERR_NO_SERVER,
    SNTP_ERR_SOCKET,
    SNTP_ERR_SEND,
    SNTP_ERR_TIMEOUT,
    SNTP_ERR_FORMAT
} sntp_result_t;

sntp_result_t sntp_poll_once(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
