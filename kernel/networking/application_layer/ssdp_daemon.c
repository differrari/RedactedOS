#include "ssdp_daemon.h"

#include "process/scheduler.h"
#include "std/memory.h"
#include "std/string.h"
#include "syscalls/syscalls.h"
#include "net/network_types.h"
#include "net/socket_types.h"
#include "networking/transport_layer/csocket.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/application_layer/ssdp.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/interface_manager.h"
#include "math/math.h"
#include "random/random.h"

//at the moment it's a very basic version. it's a protocol still in use but only in few cases
//it;s used in some printers, upnp, local video streaming and various other things 
//eventually if needed, reactivate the process in net_proc

typedef struct {
    uint8_t used;
    socket_handle_t sock;
    uint32_t due_ms;
    net_l4_endpoint dst;
} ssdp_pending_t;

typedef struct {
    socket_handle_t sock;
    ip_version_t ver;
    uint8_t mcast_ip[16];
} ssdp_socket_entry_t;

static rng_t ssdp_rng;
static uint32_t ssdp_uptime_ms = 0;

static uint32_t ssdp_host_v4 = IPV4_MCAST_SSDP;
static uint8_t ssdp_host_v6[16];
static ssdp_socket_entry_t ssdp_sockets[MAX_L3_INTERFACES];
static uint8_t ssdp_socket_count = 0;

#define SSDP_MAX_PENDING 64
#define SSDP_RATE_WINDOW_MS 1000
#define SSDP_RATE_MAX 20
#define SSDP_NOTIFY_INTERVAL_MS 300000
#define SSDP_RECV_BURST 8

static ssdp_pending_t ssdp_pending[SSDP_MAX_PENDING];
static uint32_t ssdp_rate_window_ms = 0;
static uint32_t ssdp_rate_count = 0;

static void ssdp_schedule_response(socket_handle_t sock, const net_l4_endpoint* src, uint32_t mx_ms) {
    if (!sock || !src) return;
    for (int i = 0; i < SSDP_MAX_PENDING; ++i) {
        if (!ssdp_pending[i].used) {
            ssdp_pending[i].used = 1;
            ssdp_pending[i].sock = sock;
            ssdp_pending[i].dst = *src;
            ssdp_pending[i].due_ms = ssdp_uptime_ms + rng_between32(&ssdp_rng, 0, mx_ms);
            return;
        }
    }
}

static void ssdp_send_notify(bool alive) {
    for (uint8_t i = 0; i < ssdp_socket_count; i++) {
        socket_handle_t s = ssdp_sockets[i].sock;
        if (!s) continue;

        string msg = ssdp_build_notify(alive, ssdp_sockets[i].ver == IP_VER6);
        net_l4_endpoint dst;
        if (ssdp_sockets[i].ver == IP_VER4) make_ep(&ssdp_host_v4, 1900, IP_VER4, &dst);
        else make_ep(ssdp_host_v6, 1900, IP_VER6, &dst);
        (void)send_to_socket(s, &dst, (void*)msg.data, msg.length);
        string_free(msg);
    }
}

int ssdp_daemon_entry(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    rng_init_random(&ssdp_rng);

    ipv6_make_multicast(0x02, IPV6_MCAST_SSDP, NULL, ssdp_host_v6);

    uint8_t n_if = l2_interface_count();
    for (uint8_t i = 0; i < n_if && ssdp_socket_count < MAX_L3_INTERFACES; i++) {
        l2_interface_t *l2 = l2_interface_at(i);
        if (!l2 || !l2->is_up) continue;

        for (uint8_t j = 0; j < MAX_IPV4_PER_INTERFACE && ssdp_socket_count < MAX_L3_INTERFACES; j++) {
            l3_ipv4_interface_t *v4 = l2->l3_v4[j];
            if (!ipv4_l3_is_ready(v4) || v4->is_localhost) continue;

            socket_handle_t s = create_socket(PROTO_UDP, &(SocketOptions){.flags = SOCK_OPT_TTL | SOCK_OPT_NONBLOCK, .ttl = 2});
            if (!s) continue;

            SockBindSpec spec;
            memset(&spec, 0, sizeof(spec));
            spec.kind = BIND_L3;
            spec.ver = IP_VER4;
            spec.l3_id = v4->l3_id;

            net_l4_endpoint group;
            memset(&group, 0, sizeof(group));
            group.ver = IP_VER4;
            group.port = 1900;
            memcpy(group.ip, &ssdp_host_v4, 4);

            if (bind_socket(s, &spec, 1900) != SOCK_OK || set_socket_option(s, SOCK_OPT_MCAST_JOIN, &group, sizeof(group)) != SOCK_OK) {
                close_socket(s);
                continue;
            }

            ssdp_sockets[ssdp_socket_count].sock = s;
            ssdp_sockets[ssdp_socket_count].ver = IP_VER4;
            memcpy(ssdp_sockets[ssdp_socket_count].mcast_ip, &ssdp_host_v4, 4);
            ssdp_socket_count++;
        }

        for (uint8_t j = 0; j < MAX_IPV6_PER_INTERFACE && ssdp_socket_count < MAX_L3_INTERFACES; j++) {
            l3_ipv6_interface_t *v6 = l2->l3_v6[j];
            if (!ipv6_l3_is_ready(v6) || v6->is_localhost) continue;

            socket_handle_t s = create_socket(PROTO_UDP, &(SocketOptions){.flags = SOCK_OPT_TTL | SOCK_OPT_NONBLOCK, .ttl = 2});
            if (!s) continue;

            SockBindSpec spec;
            memset(&spec, 0, sizeof(spec));
            spec.kind = BIND_L3;
            spec.ver = IP_VER6;
            spec.l3_id = v6->l3_id;

            net_l4_endpoint group;
            memset(&group, 0, sizeof(group));
            group.ver = IP_VER6;
            group.port = 1900;
            memcpy(group.ip, ssdp_host_v6, 16);

            if (bind_socket(s, &spec, 1900) != SOCK_OK || set_socket_option(s, SOCK_OPT_MCAST_JOIN, &group, sizeof(group)) != SOCK_OK) {
                close_socket(s);
                continue;
            }

            ssdp_sockets[ssdp_socket_count].sock = s;
            ssdp_sockets[ssdp_socket_count].ver = IP_VER6;
            memcpy(ssdp_sockets[ssdp_socket_count].mcast_ip, ssdp_host_v6, 16);
            ssdp_socket_count++;
        }
    }

    if (!ssdp_socket_count) return 1;

    ssdp_send_notify(true);
    msleep(100);
    ssdp_send_notify(true);
    msleep(100);
    ssdp_send_notify(true);

    uint32_t notify_ms = 0;
    const uint32_t tick_ms = 50;

    while (1) {
        notify_ms += tick_ms;
        if (notify_ms >= SSDP_NOTIFY_INTERVAL_MS) {
            notify_ms = 0;
            ssdp_send_notify(true);
        }

        char buf[2048];
        net_l4_endpoint src = (net_l4_endpoint){0};

        for (uint8_t sidx = 0; sidx < ssdp_socket_count; sidx++) {
            socket_handle_t s = ssdp_sockets[sidx].sock;
            for (int i = 0; i < SSDP_RECV_BURST; ++i) {
                int64_t r = receive_from_socket(s, buf, sizeof(buf) - 1, &src);
                if (r == SOCK_ERR_WOULDBLOCK) break;
                if (r < 0) break;
                if (!r) continue;
                buf[r] = 0;
                if (ssdp_is_msearch(buf, (int)r)) ssdp_schedule_response(s, &src, ssdp_parse_mx_ms(buf, (int)r));
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
            if (ssdp_pending[i].sock) (void)send_to_socket(ssdp_pending[i].sock, &ssdp_pending[i].dst, (void*)resp.data, resp.length);
            string_free(resp);
            break;
        }

        msleep(tick_ms);
    }
}
