#include "dns_daemon.h"
#include "mdns_responder.h"
#include "dns_sd.h"
#include "process/scheduler.h"
#include "syscalls/syscalls.h"
#include "net/socket_types.h"
#include "networking/transport_layer/csocket.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/interface_manager.h"
#include "std/memory.h"

static mdns_tx_target_t g_mdns[MAX_L3_INTERFACES];
static uint16_t g_mdns_count = 0;

socket_handle_t mdns_socket_handle_for(ip_version_t ver){
    for (uint16_t i = 0; i < g_mdns_count; i++) {
        if (g_mdns[i].ver == ver) return g_mdns[i].sock;
    }
    return 0;
}

socket_handle_t mdns_socket_handle_for_l3(l3_id_t l3_id) {
    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(l3_id);
    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(l3_id);
    uint32_t generation = v4 ? v4->generation : v6 ? v6->generation : 0;
    if (!generation) return 0;
    for (uint16_t i = 0; i < g_mdns_count; i++) if (g_mdns[i].l3_id == l3_id && g_mdns[i].l3_generation == generation) return g_mdns[i].sock;
    return 0;
}

static void mdns_prune_sockets(void) {
    uint16_t out = 0;
    for (uint16_t i = 0; i < g_mdns_count; i++) {
        bool valid = false;
        if (g_mdns[i].sock && g_mdns[i].ver == IP_VER4) {
            l3_ipv4_interface_t *v4 = l3_ipv4_find_by_id(g_mdns[i].l3_id);
            valid = ipv4_l3_is_ready(v4) && !v4->is_localhost && v4->generation == g_mdns[i].l3_generation;
        } else if (g_mdns[i].sock && g_mdns[i].ver == IP_VER6) {
            l3_ipv6_interface_t *v6 = l3_ipv6_find_by_id(g_mdns[i].l3_id);
            valid = ipv6_l3_is_ready(v6) && !v6->is_localhost && v6->generation == g_mdns[i].l3_generation;
        }
        if (!valid) {
            if (g_mdns[i].sock) close_socket(g_mdns[i].sock);
            continue;
        }
        if (out != i) g_mdns[out] = g_mdns[i];
        out++;
    }
    g_mdns_count = out;
}

static void mdns_open_sockets(const uint8_t *group4, const uint8_t *group6) {
    if (!group4 || !group6) return;

    uint8_t n_if = l2_interface_count();
    for (uint8_t i = 0; i < n_if && g_mdns_count < MAX_L3_INTERFACES; i++) {
        l2_interface_t *l2 = l2_interface_at(i);
        if (!l2 || !l2->is_up) continue;

        for (uint8_t j = 0; j < MAX_IPV4_PER_INTERFACE && g_mdns_count < MAX_L3_INTERFACES; j++) {
            l3_ipv4_interface_t *v4 = l2->l3_v4[j];
            if (!ipv4_l3_is_ready(v4) || v4->is_localhost) continue;
            bool have_socket = false;
            for (uint16_t k = 0; k < g_mdns_count; k++) {
                if (g_mdns[k].sock && g_mdns[k].ver == IP_VER4 && g_mdns[k].l3_id == v4->l3_id && g_mdns[k].l3_generation == v4->generation) {
                    have_socket = true;
                    break;
                }
            }
            if (have_socket) continue;

            socket_handle_t s = create_socket(PROTO_UDP, &(SocketOptions){.flags = SOCK_OPT_TTL | SOCK_OPT_NONBLOCK, .ttl = 255});
            if (!s) continue;

            SockBindSpec spec = {.kind = BIND_L3, .ver = IP_VER4, .l3_id = v4->l3_id};
            net_l4_endpoint group = {.ver = IP_VER4, .port = DNS_MDNS_PORT};
            memcpy(group.ip, group4, 4);

            if (bind_socket(s, &spec, DNS_MDNS_PORT) != SOCK_OK || set_socket_option(s, SOCK_OPT_MCAST_JOIN, &group, sizeof(group)) != SOCK_OK) {
                close_socket(s);
                continue;
            }

            g_mdns[g_mdns_count].sock = s;
            g_mdns[g_mdns_count].ver = IP_VER4;
            g_mdns[g_mdns_count].l3_id = v4->l3_id;
            g_mdns[g_mdns_count].l3_generation = v4->generation;
            memcpy(g_mdns[g_mdns_count].mcast_ip, group4, 4);
            g_mdns_count++;
        }

        for (uint8_t j = 0; j < MAX_IPV6_PER_INTERFACE && g_mdns_count < MAX_L3_INTERFACES; j++) {
            l3_ipv6_interface_t *v6 = l2->l3_v6[j];
            if (!ipv6_l3_is_ready(v6) || v6->is_localhost) continue;
            bool have_socket = false;
            for (uint16_t k = 0; k < g_mdns_count; k++) {
                if (g_mdns[k].sock && g_mdns[k].ver == IP_VER6 && g_mdns[k].l3_id == v6->l3_id && g_mdns[k].l3_generation == v6->generation) {
                    have_socket = true;
                    break;
                }
            }
            if (have_socket) continue;

            socket_handle_t s = create_socket(PROTO_UDP, &(SocketOptions){.flags = SOCK_OPT_TTL | SOCK_OPT_NONBLOCK, .ttl = 255});
            if (!s) continue;

            SockBindSpec spec = {.kind = BIND_L3, .ver = IP_VER6, .l3_id = v6->l3_id};
            net_l4_endpoint group = {.ver = IP_VER6, .port = DNS_MDNS_PORT};
            memcpy(group.ip, group6, 16);

            if (bind_socket(s, &spec, DNS_MDNS_PORT) != SOCK_OK || set_socket_option(s, SOCK_OPT_MCAST_JOIN, &group, sizeof(group)) != SOCK_OK) {
                close_socket(s);
                continue;
            }

            g_mdns[g_mdns_count].sock = s;
            g_mdns[g_mdns_count].ver = IP_VER6;
            g_mdns[g_mdns_count].l3_id = v6->l3_id;
            g_mdns[g_mdns_count].l3_generation = v6->generation;
            memcpy(g_mdns[g_mdns_count].mcast_ip, group6, 16);
            g_mdns_count++;
        }
    }
}

int dns_deamon_entry(int argc, char* argv[]){
    (void)argc; (void)argv;

    uint32_t mdns_v4 = IPV4_MCAST_MDNS;
    uint8_t mdns_v4_addr[4];
    uint8_t mdns_v6[16];
    memcpy(mdns_v4_addr, &mdns_v4, 4);
    ipv6_make_multicast(0x02, IPV6_MCAST_MDNS, 0, mdns_v6);

    mdns_prune_sockets();
    mdns_open_sockets(mdns_v4_addr, mdns_v6);

    const uint32_t tick_ms = 100;
    uint32_t last_socket_sync_ms = (uint32_t)get_time();
    for(;;) {
        uint32_t now_ms = (uint32_t)get_time();
        if (now_ms - last_socket_sync_ms >= 1000) {
            last_socket_sync_ms = now_ms;
            mdns_prune_sockets();
            mdns_open_sockets(mdns_v4_addr, mdns_v6);
        }

        uint8_t buf[900];
        net_l4_endpoint src;

        for (uint16_t sidx = 0; sidx < g_mdns_count; sidx++) {
            socket_handle_t s = g_mdns[sidx].sock;
            for (int i = 0; i < 64; i++) {
                memset(&src, 0, sizeof(src));
                int64_t r = receive_from_socket(s, buf, sizeof(buf), &src);
                if (r == SOCK_ERR_WOULDBLOCK) break;
                if (r < 0) break;
                if (!r) continue;
                mdns_responder_handle_query(s, g_mdns[sidx].ver, g_mdns[sidx].mcast_ip, buf, (uint32_t)r, &src);
            }
        }

        mdns_responder_tick_multi(g_mdns, g_mdns_count);
        msleep(tick_ms);
    }
    return 1;
}