#include "dns_daemon.h"
#include "process/scheduler.h"
#include "syscalls/syscalls.h"
#include "net/transport_layer/socket_types.h"
#include "dns.h"
#include "net/internet_layer/ipv6_utils.h"

#define MDNS_PORT 5353

static uint16_t g_pid_dnsd = 0xFFFF;
static socket_handle_t g_sock = 0;

static socket_handle_t g_sock_mdns4 = 0;
static socket_handle_t g_sock_mdns6 = 0;

uint16_t dns_get_pid(void){ return g_pid_dnsd; }
bool dns_is_running(void){ return g_pid_dnsd != 0xFFFF; }
void dns_set_pid(uint16_t p){ g_pid_dnsd = p; }
socket_handle_t dns_socket_handle(void){ return g_sock; }

socket_handle_t mdns_socket_handle_v4(void){ return g_sock_mdns4; }
socket_handle_t mdns_socket_handle_v6(void){ return g_sock_mdns6; }

int dns_deamon_entry(int argc, char* argv[]){
    (void)argc; (void)argv;
    dns_set_pid(get_current_proc_pid());
    g_sock = udp_socket_create(SOCK_ROLE_CLIENT, g_pid_dnsd, NULL);

    uint32_t mdns_v4 = IPV4_MCAST_MDNS;
    uint8_t mdns_v6[16];
    ipv6_make_multicast(0x02, IPV6_MCAST_MDNS, 0, mdns_v6);

    SocketExtraOptions opt4;
    memset(&opt4, 0, sizeof(opt4));
    opt4.flags = SOCK_OPT_MCAST_JOIN | SOCK_OPT_TTL;
    opt4.ttl = 255;
    opt4.mcast_ver = IP_VER4;
    memcpy(opt4.mcast_group, &mdns_v4, 4);

    SocketExtraOptions opt6;
    memset(&opt6, 0, sizeof(opt6));
    opt6.flags = SOCK_OPT_MCAST_JOIN | SOCK_OPT_TTL;
    opt6.ttl = 255;
    opt6.mcast_ver = IP_VER6;
    memcpy(opt6.mcast_group, mdns_v6, 16);

    g_sock_mdns4 = udp_socket_create(SOCK_ROLE_SERVER, g_pid_dnsd, &opt4);
    g_sock_mdns6 = udp_socket_create(SOCK_ROLE_SERVER, g_pid_dnsd, &opt6);

    SockBindSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = BIND_ANY;

    if (g_sock_mdns4) {
        if (socket_bind_udp_ex(g_sock_mdns4, &spec, MDNS_PORT) != SOCK_OK) {
            socket_destroy_udp(g_sock_mdns4);
            g_sock_mdns4 = 0;
        }
    }

    if (g_sock_mdns6) {
        if (socket_bind_udp_ex(g_sock_mdns6, &spec, MDNS_PORT) != SOCK_OK) {
            socket_destroy_udp(g_sock_mdns6);
            g_sock_mdns6 = 0;
        }
    }

    for(;;){
        dns_cache_tick(250);
        sleep(250);
    }
    return 1;
}