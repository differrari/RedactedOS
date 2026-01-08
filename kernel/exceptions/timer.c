#include "timer.h"
#include "math/math.h"

#define TIMER_SLEW_MAX_PPM 500
#define TIMER_FREQ_MAX_PPM 500

static int g_sync = 0;

static uint64_t g_wall_base_mono_us = 0;
static int64_t g_wall_base_unix_us = 0;
static int32_t g_freq_ppm = 0;
static int64_t g_slew_rem_us = 0;

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

static inline void timer_enable() {
    uint64_t val = 1;
    asm volatile ("msr cntp_ctl_el0, %0" :: "r"(val));
    asm volatile ("msr cntkctl_el1, %0" :: "r"(val));
}

void permanent_disable_timer(){
    uint64_t ctl = 0;
    asm volatile ("msr cntp_ctl_el0, %0" :: "r"(ctl));
}

void timer_init(uint64_t msecs) {
    timer_reset(msecs);
    timer_enable();

    g_wall_base_mono_us = timer_now_usec();
    g_wall_base_unix_us = 0;
    g_freq_ppm = 0;
    g_slew_rem_us = 0;
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

uint64_t timer_now_msec() {
    uint64_t ticks = timer_now();
    uint64_t freq = rd_cntfrq_el0();
    return (ticks * 1000) / freq;
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
        int64_t adj = dt +(dt * (int64_t)g_freq_ppm)/1000000LL;
        base += adj;

        int64_t max_slew = (dt * (int64_t)TIMER_SLEW_MAX_PPM) / 1000000LL;
        if (max_slew < 1)max_slew = 1;

        if (g_slew_rem_us) {
            int64_t apply = clamp_i64(g_slew_rem_us, -max_slew, max_slew);
            g_slew_rem_us -= apply;
            base += apply;
        }

        g_wall_base_mono_us = mono_now_us;
        g_wall_base_unix_us = base;
        return base;
    }

    return g_wall_base_unix_us;
}

uint64_t timer_wall_time_us(void) {
    return (uint64_t)wall_advance_to(timer_now_usec());
}

uint64_t timer_unix_time_us(void) {
    if (!g_sync) return 0;
    int64_t u = wall_advance_to( timer_now_usec());
    if (u < 0) return 0;
    return (uint64_t)u;
}

void timer_sync_set_unix_us(uint64_t unix_us) {
    uint64_t now_us = timer_now_usec();
    g_wall_base_mono_us = now_us;
    g_wall_base_unix_us= (int64_t)unix_us;
    g_slew_rem_us = 0;
    g_sync = 1;
}

void timer_sync_slew_us(int64_t delta_us){
    const int64_t cap = 60LL * 1000000LL;
    int64_t v = g_slew_rem_us + delta_us;
    g_slew_rem_us = clamp_i64(v, -cap, cap);
}

void timer_sync_set_freq_ppm(int32_t ppm) {
    g_freq_ppm = clamp_i64((int32_t)ppm, -TIMER_FREQ_MAX_PPM, TIMER_FREQ_MAX_PPM);
}

int32_t timer_sync_get_freq_ppm(void) {
    return g_freq_ppm;
}

void timer_apply_sntp_sample_us(uint64_t server_unix_us) {
    timer_sync_set_unix_us(server_unix_us);
}

int timer_is_synchronised(void) {
    return g_sync;
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
    if (g_sync) return -1;
    uint64_t now_us = timer_now_usec();
    g_wall_base_mono_us = now_us;
    g_wall_base_unix_us = (int64_t)(unix_ms * 1000ULL);
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

static void fmt2u(uint64_t v, char out[3]){ //maybe move this in a helper file
    out[0] = (char)('0' + (v/10u)%10u);
    out[1] = (char)('0' + (v%10u));
    out[2] = '\0';
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
    uint16_t Y = dt->year;
    buf[0] = (char)('0' + (Y/1000)%10);
    buf[1] = (char)('0' + (Y/100)%10);
    buf[2] = (char)('0' + (Y/10)%10);
    buf[3] = (char)('0' + (Y%10));
    buf[4] = '-';

    char mm[3], dd[3], HH[3], MM[3], SS[3];
    fmt2u(dt->month, mm);
    fmt2u(dt->day, dd);
    fmt2u(dt->hour, HH);
    fmt2u(dt->minute, MM);
    fmt2u(dt->second, SS);

    buf[5] = mm[0]; buf[6] = mm[1];
    buf[7] = '-';
    buf[8] = dd[0]; buf[9] = dd[1];
    buf[10] = ' ';
    buf[11] = HH[0]; buf[12] = HH[1];
    buf[13] = ':';
    buf[14] = MM[0]; buf[15] = MM[1];
    buf[16] = ':';
    buf[17] = SS[0]; buf[18] = SS[1];
    buf[19] = '\0';
}
