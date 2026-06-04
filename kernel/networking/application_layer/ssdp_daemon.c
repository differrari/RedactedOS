#include "ssdp_daemon.h"

#include "process/scheduler.h"
#include "std/memory.h"
#include "std/string.h"
#include "syscalls/syscalls.h"
#include "net/network_types.h"
#include "networking/transport_layer/csocket.h"
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
#define SSDP_RECV_BURST 8

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
    if (((s4).id && (s4).protocol != PROTO_NONE)) {
        string msg = ssdp_build_notify(alive, false);
        net_l4_endpoint dst;
        make_ep(ssdp_host_v4, 1900, IP_VER4, &dst);
        (void)send_to_socket(&s4, &dst, (void*)msg.data, msg.length);
        string_free(msg);
    }

    if (((s6).id && (s6).protocol != PROTO_NONE)) {
        string msg = ssdp_build_notify(alive, true);
        net_l4_endpoint dst = (net_l4_endpoint){0};
        dst.ver = IP_VER6;
        memcpy(dst.ip, ssdp_host_v6, 16);
        dst.port = 1900;
        (void)send_to_socket(&s6, &dst, (void*)msg.data, msg.length);
        string_free(msg);
    }
}

int ssdp_daemon_entry(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    uint64_t virt_timer;
    asm volatile ("mrs %0, cntvct_el0" : "=r"(virt_timer));
    rng_seed(&ssdp_rng, virt_timer);

    ipv6_make_multicast(0x02, IPV6_MCAST_SSDP, NULL, ssdp_host_v6);

    SocketExtraOptions opt = (SocketExtraOptions){0};
    opt.flags = SOCK_OPT_MCAST_JOIN;
    opt.mcast_count = 2;
    opt.mcast_groups[0].ver = IP_VER4;
    opt.mcast_groups[0].port = 1900;
    memcpy(opt.mcast_groups[0].ip, &ssdp_host_v4, 4);
    opt.mcast_groups[1].ver = IP_VER6;
    opt.mcast_groups[1].port = 1900;
    memcpy(opt.mcast_groups[1].ip, ssdp_host_v6, 16);
    socket_handle_t s = {0};
    create_socket(SOCKET_SERVER, PROTO_UDP, &opt,&s);

    struct SockBindSpec spec = (struct SockBindSpec){0};
    spec.kind = BIND_ANY;
    if (((s).id && (s).protocol != PROTO_NONE) && bind_socket_spec(&s, &spec, 1900) < 0) {
        close_socket(&s);
        s = ((socket_handle_t){0});
    }

    socket_handle_t s4 = s;
    socket_handle_t s6 = s;

    if (!((s4).id && (s4).protocol != PROTO_NONE) && !((s6).id && (s6).protocol != PROTO_NONE)) return 1;

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

        if (((s).id && (s).protocol != PROTO_NONE)) {
            for (int i = 0; i < SSDP_RECV_BURST; ++i) {
                int64_t r = receive_from_socket(&s, buf, sizeof(buf) - 1, &src);
                if (r <= 0) break;
                buf[r] = 0;
                if (ssdp_is_msearch(buf, (int)r)) ssdp_schedule_response(&src, ssdp_parse_mx_ms(buf, (int)r));
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
            if (((sock).id && (sock).protocol != PROTO_NONE)) (void)send_to_socket(&sock, &ssdp_pending[i].dst, (void*)resp.data, resp.length);
            string_free(resp);
            break;
        }

        msleep(tick_ms);
    }
}
