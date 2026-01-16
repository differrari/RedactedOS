#include "sntp_daemon.h"//deprecated, use ntp
#include "sntp.h"
#include "exceptions/timer.h"
#include "process/scheduler.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv4_utils.h"

#include "networking/interface_manager.h"
#include "syscalls/syscalls.h"


static uint16_t g_pid_sntp = 0xFFFF;
static socket_handle_t g_sock = 0;

uint16_t sntp_get_pid(void){ return g_pid_sntp; }
bool sntp_is_running(void){ return g_pid_sntp != 0xFFFF; }
void sntp_set_pid(uint16_t p){ g_pid_sntp = p; }
socket_handle_t sntp_socket_handle(void){ return g_sock; }

#define SNTP_POLL_INTERVAL_MS (10u * 60u * 1000u)
#define SNTP_QUERY_TIMEOUT_MS 1200u
#define SNTP_BOOTSTRAP_MAX_RETRY 5u

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

int sntp_daemon_entry(int argc, char* argv[]){
    (void)argc; (void)argv;
    g_pid_sntp = (uint16_t)get_current_proc_pid();
    g_sock = udp_socket_create(0, g_pid_sntp, NULL);
    sntp_set_pid(get_current_proc_pid());
    uint32_t attempts = 0;
    while (attempts < SNTP_BOOTSTRAP_MAX_RETRY){
        if (!any_ipv4_configured_nonlocal()){
            msleep(500);
            continue;
        }
        sntp_result_t r = sntp_poll_once(SNTP_QUERY_TIMEOUT_MS);
        if (r == SNTP_OK){
            break;
        }
        attempts++;
        uint32_t backoff_ms = (1u << (attempts <= 4 ? attempts : 4)) * 250u;
        uint32_t jitter = backoff_ms / 10u;
        msleep(backoff_ms + (jitter / 2u));
    }
    for(;;){
        sntp_poll_once(SNTP_QUERY_TIMEOUT_MS);
        msleep(SNTP_POLL_INTERVAL_MS);
    }
    return 1;
}
