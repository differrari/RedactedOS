#include "ssdp_daemon.h"

#include "process/scheduler.h"
#include "std/memory.h"
#include "std/string.h"
#include "syscalls/syscalls.h"
#include "net/network_types.h"
#include "networking/transport_layer/csocket_udp.h"
#include "networking/application_layer/ssdp.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "math/math.h"
#include "networking/transport_layer/trans_utils.h"

//at the moment it's a very basic version. it's a protocol still in use but only in few cases
//it;s used in some printers, upnp, local video streaming and various other things 
//eventually if needed, reactivate the process in net_proc

typedef struct {
    uint8_t used;
    uint32_t due_ms;
    net_l4_endpoint dst;
} ssdp_pending_t;

static rng_t ssdp_rng;
static uint32_t ssdp_uptime_ms = 0;

static uint32_t ssdp_host_v4 = IPV4_MCAST_SSDP;
static uint8_t ssdp_host_v6[16];

#define SSDP_MAX_PENDING 64
#define SSDP_RATE_WINDOW_MS 1000
#define SSDP_RATE_MAX 20
#define SSDP_NOTIFY_INTERVAL_MS 300000

static ssdp_pending_t ssdp_pending[SSDP_MAX_PENDING];
static uint32_t ssdp_rate_window_ms = 0;
static uint32_t ssdp_rate_count = 0;

static void ssdp_schedule_response(const net_l4_endpoint* src, uint32_t mx_ms) {
    if (!src) return;
    for (int i = 0; i < SSDP_MAX_PENDING; ++i) {
        if (!ssdp_pending[i].used) {
            ssdp_pending[i].used = 1;
            ssdp_pending[i].dst = *src;
            ssdp_pending[i].due_ms = ssdp_uptime_ms + rng_between32(&ssdp_rng, 0, mx_ms);
            return;
        }
    }
}

static void ssdp_send_notify(socket_handle_t s4, socket_handle_t s6, bool alive) {
    if (s4) {
        string msg = ssdp_build_notify(alive, false);
        net_l4_endpoint dst;
        make_ep(ssdp_host_v4, 1900, IP_VER4, &dst);
        (void)socket_sendto_udp_ex(s4, DST_ENDPOINT, &dst, 0, msg.data, msg.length);
        free_sized(msg.data, msg.mem_length);
    }

    if (s6) {
        string msg = ssdp_build_notify(alive, true);
        net_l4_endpoint dst = (net_l4_endpoint){0};
        dst.ver = IP_VER6;
        memcpy(dst.ip, ssdp_host_v6, 16);
        dst.port = 1900;
        (void)socket_sendto_udp_ex(s6, DST_ENDPOINT, &dst, 0, msg.data, msg.length);
        free_sized(msg.data, msg.mem_length);
    }
}

int ssdp_daemon_entry(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    uint64_t virt_timer;
    asm volatile ("mrs %0, cntvct_el0" : "=r"(virt_timer));
    rng_seed(&ssdp_rng, virt_timer);

    ipv6_make_multicast(0x02, IPV6_MCAST_SSDP, NULL, ssdp_host_v6);

    SocketExtraOptions opt4 = (SocketExtraOptions){0};
    opt4.flags = SOCK_OPT_MCAST_JOIN;
    opt4.mcast_ver = IP_VER4;
    memcpy(opt4.mcast_group, &ssdp_host_v4, 4);

    SocketExtraOptions opt6 = (SocketExtraOptions){0};
    opt6.flags = SOCK_OPT_MCAST_JOIN;
    opt6.mcast_ver = IP_VER6;
    memcpy(opt6.mcast_group, ssdp_host_v6, 16);

    uint16_t pid = get_current_proc_pid();

    socket_handle_t s4 = udp_socket_create(SOCK_ROLE_SERVER, pid, &opt4);
    socket_handle_t s6 = udp_socket_create(SOCK_ROLE_SERVER, pid, &opt6);

    struct SockBindSpec spec = (struct SockBindSpec){0};
    spec.kind = BIND_ANY;
    if (s4 && socket_bind_udp_ex(s4, &spec, 1900) < 0) {
        socket_close_udp(s4);
        socket_destroy_udp(s4);
        s4 = 0;
    }
    if (s6 && socket_bind_udp_ex(s6, &spec, 1900) < 0) {
        socket_close_udp(s6);
        socket_destroy_udp(s6);
        s6 = 0;
    }

    if (!s4 && !s6) return 1;

    ssdp_send_notify(s4, s6, true);
    msleep(100);
    ssdp_send_notify(s4, s6, true);
    msleep(100);
    ssdp_send_notify(s4, s6, true);

    uint32_t notify_ms = 0;
    const uint32_t tick_ms = 50;

    while (1) {
        notify_ms += tick_ms;
        if (notify_ms >= SSDP_NOTIFY_INTERVAL_MS) {
            notify_ms = 0;
            ssdp_send_notify(s4, s6, true);
        }

        char buf[2048];
        net_l4_endpoint src = (net_l4_endpoint){0};

        if (s4) {
            int64_t r4 = socket_recvfrom_udp_ex(s4, buf, sizeof(buf) - 1, &src);
            if (r4 > 0) {
                buf[r4] = 0;
                if (ssdp_is_msearch(buf, (int)r4)) ssdp_schedule_response(&src, ssdp_parse_mx_ms(buf, (int)r4));
            }
        }

        if (s6) {
            int64_t r6 = socket_recvfrom_udp_ex(s6, buf, sizeof(buf) - 1, &src);
            if (r6 > 0) {
                buf[r6] = 0;
                if (ssdp_is_msearch(buf, (int)r6)) ssdp_schedule_response(&src, ssdp_parse_mx_ms(buf, (int)r6));
            }
        }

        ssdp_uptime_ms += tick_ms;
        ssdp_rate_window_ms += tick_ms;
        if (ssdp_rate_window_ms >= SSDP_RATE_WINDOW_MS) {
            ssdp_rate_window_ms = 0;
            ssdp_rate_count = 0;
        }

        for (int i = 0; i < SSDP_MAX_PENDING; ++i) {
            if (!ssdp_pending[i].used) continue;
            if (ssdp_uptime_ms < ssdp_pending[i].due_ms) continue;
            if (ssdp_rate_count >= SSDP_RATE_MAX) break;

            ssdp_rate_count += 1;
            ssdp_pending[i].used = 0;

            string resp = ssdp_build_search_response();
            socket_handle_t sock = (ssdp_pending[i].dst.ver == IP_VER6) ? s6 : s4;
            if (sock) (void)socket_sendto_udp_ex(sock, DST_ENDPOINT, &ssdp_pending[i].dst, 0, resp.data, resp.length);
            free_sized(resp.data, resp.mem_length);
            break;
        }

        msleep(tick_ms);
    }
}
