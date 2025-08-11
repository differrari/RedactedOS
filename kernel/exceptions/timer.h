#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void timer_init(uint64_t msecs);
void timer_reset();

void virtual_timer_reset(uint64_t smsecs);
void virtual_timer_enable();
uint64_t virtual_timer_remaining_msec();

uint64_t timer_now();
uint64_t timer_now_msec();
uint64_t timer_now_usec(void);

void timer_apply_sntp_sample_us(uint64_t server_unix_us);
int timer_is_synchronised(void);
uint64_t timer_unix_time_ms(void);

void timer_set_timezone_minutes(int32_t minutes);
int32_t timer_get_timezone_minutes(void);

uint64_t timer_local_time_ms(void);
int timer_set_manual_unix_time_ms(uint64_t unix_ms);

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} DateTime;

void timer_unix_ms_to_datetime(uint64_t unix_ms, int use_local, DateTime* out);
uint64_t timer_datetime_to_unix_ms(const DateTime* dt, int is_local);
int timer_now_datetime(DateTime* out, int use_local);
void timer_datetime_to_string(const DateTime* dt, char* buf, uint32_t buflen);

void permanent_disable_timer();

#ifdef __cplusplus
}
#endif
