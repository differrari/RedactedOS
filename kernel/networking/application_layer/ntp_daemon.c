#include "ntp_daemon.h"
#include "ntp.h"
#include "exceptions/timer.h"
#include "process/scheduler.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/interface_manager.h"
#include "syscalls/syscalls.h"

static uint16_t g_pid_ntp = 0xFFFF;
static socket_handle_t g_sock = 0;

uint16_t ntp_get_pid(void){ return g_pid_ntp; }
bool ntp_is_running(void){ return g_pid_ntp != 0xFFFF; }
void ntp_set_pid(uint16_t p){ g_pid_ntp = p; }
socket_handle_t ntp_socket_handle(void){ return g_sock; }

#define NTP_POLL_INTERVAL_MS 60000u
#define NTP_QUERY_TIMEOUT_MS 1200u
#define NTP_BOOTSTRAP_MAX_RETRY 8u
#define NTP_WARMUP_INTERVAL_MS 400u

static bool any_ipv4_configured_nonlocal(void){
    uint8_t n = l2_interface_count();
    for (uint8_t i = 0; i < n; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2 || !l2->is_up) continue;
        for (int s = 0; s < MAX_IPV4_PER_INTERFACE; s++) {
            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
            if (!v4) continue;
            if (v4->mode == IPV4_CFG_DISABLED) continue;
            if (!v4->ip) continue;
            if (v4->is_localhost) continue;
            if (ipv4_is_loopback(v4->ip)) continue;
            return true;
        }
    }
    return false;
}

int ntp_daemon_entry(int argc, char* argv[]){
    (void)argc;
    (void)argv;

    g_pid_ntp = get_current_proc_pid();
    g_sock = udp_socket_create(0, g_pid_ntp, NULL);
    ntp_set_pid(get_current_proc_pid());

    uint32_t attempts = 0;
    while (attempts < NTP_BOOTSTRAP_MAX_RETRY) {
        if (!any_ipv4_configured_nonlocal()) {
            msleep(500);
            continue;
        }
        ntp_result_t r = ntp_poll_once(NTP_QUERY_TIMEOUT_MS);
        if (r == NTP_OK) break;
        attempts++;
        uint32_t backoff_ms = (1<<(attempts <= 4 ? attempts : 4)) * 250u;
        uint32_t jitter = backoff_ms / 10u;
        msleep(backoff_ms + (jitter / 2u));
    }

    while (any_ipv4_configured_nonlocal() && ntp_max_filter_count() < NTP_FILTER_N) {
        ntp_poll_once(NTP_QUERY_TIMEOUT_MS);
        msleep(NTP_WARMUP_INTERVAL_MS);
    }

    for (;;) {
        ntp_poll_once(NTP_QUERY_TIMEOUT_MS);
        msleep(NTP_POLL_INTERVAL_MS);
    }

    return 1;
}
