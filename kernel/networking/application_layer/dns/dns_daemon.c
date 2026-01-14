#include "dns_daemon.h"
#include "mdns_responder.h"
#include "dns_cache.h"
#include "dns_sd.h"
#include "process/scheduler.h"
#include "syscalls/syscalls.h"
#include "net/socket_types.h"
#include "networking/transport_layer/csocket_udp.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "std/memory.h"
#include "net/socket_types.h"

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

static socket_handle_t mdns_create_socket(ip_version_t ver, const void* group) {
    SocketExtraOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.flags = SOCK_OPT_MCAST_JOIN | SOCK_OPT_TTL;
    opt.ttl = 255;
    opt.mcast_ver = ver;
    if(ver == IP_VER4) memcpy(opt.mcast_group, group, 4);
    else memcpy(opt.mcast_group, group, 16);

    socket_handle_t s = udp_socket_create(SOCK_ROLE_SERVER, g_pid_dnsd, &opt);
    if(!s) return 0;

    SockBindSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = BIND_ANY;

    if(socket_bind_udp_ex(s, &spec, DNS_SD_MDNS_PORT) != SOCK_OK){
        socket_destroy_udp(s);
        return 0;
    }

    return s;
}

int dns_deamon_entry(int argc, char* argv[]){
    (void)argc; (void)argv;
    dns_set_pid(get_current_proc_pid());
    g_sock = udp_socket_create(SOCK_ROLE_CLIENT, g_pid_dnsd, NULL);

    uint32_t mdns_v4 = IPV4_MCAST_MDNS;
    uint8_t mdns_v6[16];
    ipv6_make_multicast(0x02, IPV6_MCAST_MDNS, 0, mdns_v6);

    g_sock_mdns4 = mdns_create_socket(IP_VER4, &mdns_v4);
    g_sock_mdns6 = mdns_create_socket(IP_VER6, mdns_v6);

    uint32_t tick_ms = 100;
    for(;;) {
        dns_cache_tick(tick_ms);
        uint8_t buf[900];
        net_l4_endpoint src;

        if (g_sock_mdns4) {
            memset(&src, 0, sizeof(src));
            int64_t r4 = socket_recvfrom_udp_ex(g_sock_mdns4, buf, sizeof(buf), &src);
            if(r4 > 0) mdns_responder_handle_query(g_sock_mdns4, IP_VER4, (const uint8_t*)&mdns_v4, buf, (uint32_t)r4, &src);
        }

        if (g_sock_mdns6) {
            memset(&src, 0, sizeof(src));
            int64_t r6 = socket_recvfrom_udp_ex(g_sock_mdns6, buf, sizeof(buf), &src);
            if(r6 > 0) mdns_responder_handle_query(g_sock_mdns6, IP_VER6, mdns_v6, buf, (uint32_t)r6, &src);
        }

        mdns_responder_tick(g_sock_mdns4,g_sock_mdns6,(const uint8_t*)&mdns_v4,mdns_v6);
        msleep(tick_ms);
    }
    return 1;
}