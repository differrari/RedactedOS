#include "dns_daemon.h"
#include "process/scheduler.h"
#include "syscalls/syscalls.h"
#include "net/socket_types.h"

static uint16_t g_pid_dnsd = 0xFFFF;
static socket_handle_t g_sock = 0;

uint16_t dns_get_pid(void){ return g_pid_dnsd; }
bool dns_is_running(void){ return g_pid_dnsd != 0xFFFF; }
void dns_set_pid(uint16_t p){ g_pid_dnsd = p; }
socket_handle_t dns_socket_handle(void){ return g_sock; }

int dns_deamon_entry(int argc, char* argv[]){
    (void)argc; (void)argv;
    dns_set_pid(get_current_proc_pid());
    g_sock = udp_socket_create(SOCK_ROLE_CLIENT, g_pid_dnsd);
    for(;;){ msleep(250); }
    return 1;
}
