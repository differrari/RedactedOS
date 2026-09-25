#include "timer.h"
#include "irq.h"
#include "math/math.h"

#define TIMER_SLEW_MAX_PPM 500
#define TIMER_FREQ_MAX_PPM 500

static int g_sync = 0;

static uint64_t g_wall_base_mono_us = 0;
static int64_t g_wall_base_unix_us = 0;
static int32_t g_freq_ppm = 0;
static int64_t g_freq_frac = 0;
static int64_t g_slew_rem_us = 0;
static uint64_t g_slew_frac = 0;

static int32_t g_tz_offset_min = 0;

static inline uint64_t rd_cntfrq_el0(void) {
    uint64_t v;
    asm volatile ("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

void timer_reset(uint64_t time) {
    uint64_t freq = rd_cntfrq_el0();
    uint64_t interval = (freq * time) / 1000;
    asm volatile ("msr cntp_tval_el0, %0" :: "r"(interval));
}

void timer_enable() {
    uint64_t val = 1;
    asm volatile ("msr cntp_ctl_el0, %0" :: "r"(val));
    asm volatile ("msr cntkctl_el1, %0" :: "r"(val));
}

void timer_disable() {
    uint64_t ctl = 0;
    asm volatile ("msr cntp_ctl_el0, %0" :: "r"(ctl));
    asm volatile ("isb");
}

void permanent_disable_timer(){
    uint64_t ctl = 0;
    asm volatile ("msr cntp_ctl_el0, %0" :: "r"(ctl));
    asm volatile ("msr cntv_ctl_el0, %0" :: "r"(ctl));
    asm volatile ("isb");
}

void timer_init(uint64_t msecs) {
    timer_reset(msecs);
    timer_enable();

    g_wall_base_mono_us = timer_now_usec();
    g_wall_base_unix_us = 0;
    g_freq_ppm = 0;
    g_freq_frac = 0;
    g_slew_rem_us = 0;
    g_slew_frac = 0;
    g_sync = 0;
}

void virtual_timer_reset(uint64_t smsecs) {
    uint64_t freq = rd_cntfrq_el0();
    uint64_t interval = (freq * smsecs) / 1000;
    asm volatile ("msr cntv_tval_el0, %0" :: "r"(interval));
}

void virtual_timer_enable() {
    uint64_t val = 1;
    asm volatile ("msr cntv_ctl_el0, %0" :: "r"(val));
}

void virtual_timer_disable() {
    uint64_t val = 0;
    asm volatile ("msr cntv_ctl_el0, %0" :: "r"(val));
}

uint64_t virtual_timer_remaining_msec() {
    uint64_t ticks;
    uint64_t freq = rd_cntfrq_el0();
    asm volatile ("mrs %0, cntv_tval_el0" : "=r"(ticks));
    return (ticks * 1000) / freq;
}

uint64_t timer_now() {
    uint64_t val;
    asm volatile ("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}
//TODO: do we want more precision since we have it?
uint64_t timer_now_msec() {
    uint64_t ticks = timer_now();
    uint64_t freq = rd_cntfrq_el0();
    uint64_t q = ticks / freq;
    uint64_t r = ticks % freq;
    return q * 1000ULL + (r * 1000ULL) / freq;
}

uint64_t timer_now_usec(void) {
    uint64_t ticks = timer_now();
    uint64_t freq = rd_cntfrq_el0();

    uint64_t q = ticks / freq;
    uint64_t r = ticks % freq;

    uint64_t us = q * 1000000ULL;
    us += (r * 1000000ULL) / freq;
    return us;
}

static int64_t wall_advance_to(uint64_t mono_now_us) {
    if (!g_wall_base_mono_us) g_wall_base_mono_us = mono_now_us;

    uint64_t dt_u = mono_now_us-g_wall_base_mono_us;
    if (dt_u) {
        int64_t dt = (int64_t)dt_u;

        int64_t base = g_wall_base_unix_us;
        int64_t dt_sec = dt / 1000000LL;
        int64_t dt_rem = dt % 1000000LL;
        int64_t freq_num = dt_rem * (int64_t)g_freq_ppm + g_freq_frac;
        int64_t freq_adj = dt_sec * (int64_t)g_freq_ppm + freq_num / 1000000LL;
        g_freq_frac = freq_num % 1000000LL;
        base += dt + freq_adj;

        if (g_slew_rem_us) {
            uint64_t slew_num = (dt_u % 1000000ULL) * (uint64_t)TIMER_SLEW_MAX_PPM + g_slew_frac;
            int64_t max_slew = (int64_t)((dt_u / 1000000ULL) * (uint64_t)TIMER_SLEW_MAX_PPM + slew_num / 1000000ULL);
            g_slew_frac = slew_num % 1000000ULL;

            if (max_slew) {
                int64_t apply = clamp_i64(g_slew_rem_us, -max_slew, max_slew);
                g_slew_rem_us -= apply;
                base += apply;
                if (!g_slew_rem_us) g_slew_frac = 0;
            }
        } else g_slew_frac = 0;

        g_wall_base_mono_us = mono_now_us;
        g_wall_base_unix_us = base;
        return base;
    }

    return g_wall_base_unix_us;
}

uint64_t timer_wall_time_us(void) {
    irq_flags_t irq = irq_save_disable();
    uint64_t us = (uint64_t)wall_advance_to(timer_now_usec());
    irq_restore(irq);
    return us;
}

uint64_t timer_unix_time_us(void) {
    irq_flags_t irq = irq_save_disable();
    if (!g_sync) {
        irq_restore(irq);
        return 0;
    }
    int64_t u = wall_advance_to( timer_now_usec());
    irq_restore(irq);
    if (u < 0) return 0;
    return (uint64_t)u;
}

void timer_sync_set_unix_us(uint64_t unix_us) {
    irq_flags_t irq = irq_save_disable();
    uint64_t now_us = timer_now_usec();
    g_wall_base_mono_us = now_us;
    g_wall_base_unix_us= (int64_t)unix_us;
    g_freq_frac = 0;
    g_slew_rem_us = 0;
    g_slew_frac = 0;
    g_sync = 1;
    irq_restore(irq);
}

void timer_sync_slew_us(int64_t delta_us){
    irq_flags_t irq = irq_save_disable();
    wall_advance_to(timer_now_usec());
    const int64_t cap = 60LL * 1000000LL;
    g_slew_rem_us = clamp_i64(delta_us, -cap, cap);
    g_slew_frac = 0;
    irq_restore(irq);
}

void timer_sync_set_freq_ppm(int32_t ppm) {
    irq_flags_t irq = irq_save_disable();
    wall_advance_to(timer_now_usec());
    g_freq_ppm = clamp_i64((int32_t)ppm, -TIMER_FREQ_MAX_PPM, TIMER_FREQ_MAX_PPM);
    g_freq_frac = 0;
    irq_restore(irq);
}

int32_t timer_sync_get_freq_ppm(void) {
    irq_flags_t irq = irq_save_disable();
    int32_t ppm = g_freq_ppm;
    irq_restore(irq);
    return ppm;
}

int timer_is_synchronised(void) {
    irq_flags_t irq = irq_save_disable();
    int sync = g_sync;
    irq_restore(irq);
    return sync;
}

uint64_t timer_unix_time_ms(void) {
    uint64_t us = timer_unix_time_us();
    if (us ==0) return 0;
    return us / 1000ULL;
}

void timer_set_timezone_minutes(int32_t minutes){
    g_tz_offset_min = minutes;
}

int32_t timer_get_timezone_minutes(void){
    return g_tz_offset_min;
}

uint64_t timer_local_time_ms(void){
    uint64_t utc_ms = timer_unix_time_ms();
    if (utc_ms == 0) return 0;
    int64_t adj = (int64_t)utc_ms + (int64_t)g_tz_offset_min * 60LL * 1000LL;
    if (adj < 0) return 0;
    return (uint64_t)adj;
}

int timer_set_manual_unix_time_ms(uint64_t unix_ms){
    irq_flags_t irq = irq_save_disable();
    if (g_sync) {
        irq_restore(irq);
        return -1;
    }
    uint64_t now_us = timer_now_usec();
    g_wall_base_mono_us = now_us;
    g_wall_base_unix_us = (int64_t)(unix_ms * 1000ULL);
    g_freq_frac = 0;
    g_slew_rem_us = 0;
    g_slew_frac = 0;
    irq_restore(irq);
    return 0;
}

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d){
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153*(m + (m > 2 ? -3 : 9)) + 2)/5 + d - 1;
    const unsigned doe = yoe * 365 + yoe/4 - yoe/100 + yoe/400 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t* y, unsigned* m, unsigned* d){
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int64_t y_full = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365*yoe + yoe/4 - yoe/100 + yoe/400);
    const unsigned mp = (5*doy + 2)/153;
    const unsigned dd = doy - (153*mp + 2)/5 + 1;
    const unsigned mm = mp + (mp < 10 ? 3 : -9);
    y_full += (mm <= 2);
    *y = y_full; *m = mm; *d = dd;
}

static inline void fmt2u(uint32_t v, char out[2]){ //maybe move this in a helper file
    out[0] = (char)('0' + (v/10u)%10u);
    out[1] = (char)('0' + (v%10u));
}

void timer_unix_ms_to_datetime(uint64_t unix_ms, int use_local, DateTime* out){
    if (!out) return;
    int64_t ms = (int64_t)unix_ms;
    if (use_local) ms += (int64_t)g_tz_offset_min * 60LL * 1000LL;
    if (ms < 0) ms = 0;

    uint64_t sec = (uint64_t)ms / 1000ULL;
    uint64_t sod = sec % 86400ULL;
    uint64_t days= sec / 86400ULL;

    int64_t Y;
    unsigned M,D;
    civil_from_days((int64_t)days, &Y, &M, &D);

    out->year = (uint16_t)Y;
    out->month = (uint8_t)M;
    out->day = (uint8_t)D;
    out->hour = (uint8_t)(sod / 3600ULL);
    out->minute = (uint8_t)((sod % 3600ULL) / 60ULL);
    out->second = (uint8_t)(sod % 60ULL);
}

uint64_t timer_datetime_to_unix_ms(const DateTime* dt, int is_local){
    if (!dt) return 0;
    uint16_t Y = dt->year;
    if (Y < 1970) Y = 1970;
    unsigned M = (dt->month >= 1 && dt->month <= 12) ? dt->month : 1;
    unsigned D = (dt->day >= 1 && dt->day <= 31) ? dt->day : 1;
    unsigned h = (dt->hour <= 23) ? dt->hour : 0;
    unsigned m = (dt->minute <= 59) ? dt->minute : 0;
    unsigned s = (dt->second <= 59) ? dt->second : 0;

    int64_t days = days_from_civil((int64_t)Y, M, D);
    uint64_t sec = (uint64_t)(days >= 0 ? days : 0) * 86400ULL + (uint64_t)h*3600ULL + (uint64_t)m*60ULL + (uint64_t)s;

    int64_t ms = (int64_t)sec * 1000LL;
    if (is_local) ms -= (int64_t)g_tz_offset_min * 60LL * 1000LL;
    if (ms < 0) ms = 0;
    return (uint64_t)ms;
}

int timer_now_datetime(DateTime* out, int use_local){
    if (!out) return 0;
    uint64_t ms = use_local ? timer_local_time_ms() : timer_unix_time_ms();
    if (ms == 0) return 0;
    timer_unix_ms_to_datetime(ms, 0, out);
    return 1;
}

void timer_datetime_to_string(const DateTime* dt, char* buf, uint32_t buflen){
    if (!dt || !buf || buflen < 20) return;
    fmt2u(dt->year/100, buf);
    fmt2u(dt->year, buf + 2);
    buf[4] = '-';

    fmt2u(dt->month, buf + 5);
    buf[7] = '-';
    fmt2u(dt->day, buf + 8);
    buf[10] = ' ';
    fmt2u(dt->hour, buf + 11);
    buf[13] = ':';
    fmt2u(dt->minute, buf + 14);
    buf[16] = ':';
    fmt2u(dt->second, buf + 17);
    buf[19] = '\0';
}
