#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NTP_OK = 0,
    NTP_ERR_NO_SERVER,
    NTP_ERR_SOCKET,
    NTP_ERR_SEND,
    NTP_ERR_TIMEOUT,
    NTP_ERR_FORMAT,
    NTP_ERR_KOD
} ntp_result_t;

typedef struct {
    int64_t offset_us;
    uint64_t delay_us;
    uint64_t dispersion_us;
    uint64_t mono_time_us;
} ntp_sample_t;

#define NTP_FILTER_N 8

ntp_result_t ntp_poll_once(uint32_t timeout_ms);
uint8_t ntp_max_filter_count(void);

#ifdef __cplusplus
}
#endif
