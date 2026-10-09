#include "ntp_daemon.h"
#include "ntp.h"
#include "process/scheduler.h"
#include "syscalls/syscalls.h"

static uint16_t g_pid_ntp = 0xFFFF;
bool ntp_is_running(void){ return g_pid_ntp != 0xFFFF; }

#define NTP_POLL_INTERVAL_MS 64000u
#define NTP_QUERY_TIMEOUT_MS 1200u
#define NTP_WARMUP_INTERVAL_MS 2000u
#define NTP_NO_SERVER_INTERVAL_MS 10000u
#define NTP_RETRY_MIN_MS 16000u
#define NTP_RETRY_MAX_MS 64000u

int ntp_daemon_entry(int argc, char* argv[]){
    (void)argc;
    (void)argv;

    g_pid_ntp = get_current_proc_pid();

    uint32_t retry_ms = NTP_RETRY_MIN_MS;

    for (;;) {
        ntp_result_t r = ntp_poll_once(NTP_QUERY_TIMEOUT_MS);
        if (r == NTP_OK) {
            retry_ms = NTP_RETRY_MIN_MS;
            msleep(ntp_max_filter_count() < NTP_FILTER_N ? NTP_WARMUP_INTERVAL_MS : NTP_POLL_INTERVAL_MS);
        } else if (r == NTP_ERR_NO_SERVER) {
            retry_ms = NTP_RETRY_MIN_MS;
            msleep(NTP_NO_SERVER_INTERVAL_MS);
        } else {
            msleep(retry_ms);
            if (retry_ms < NTP_RETRY_MAX_MS / 2) retry_ms *= 2;
            else retry_ms = NTP_RETRY_MAX_MS;
        }
    }
}
