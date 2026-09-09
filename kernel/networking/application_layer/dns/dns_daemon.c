#include "dns_daemon.h"
#include "mdns_internal.h"
#include "dns_wire.h"
#include "process/scheduler.h"
#include "kernel_processes/kprocess_loader.h"
#include "exceptions/irq.h"
#include "syscalls/syscalls.h"
#include "net/socket_types.h"
#include "networking/transport_layer/csocket.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/interface_manager.h"
#include "std/memory.h"

static mdns_tx_target_t g_mdns[MAX_L3_INTERFACES];
static uint16_t g_mdns_count = 0;
static volatile uint8_t g_dns_daemon_running;
static volatile uint8_t g_dns_daemon_pending;
static volatile uint8_t g_dns_dirty;

static uint32_t mdns_sync(const uint8_t* group4, const uint8_t* group6) {
    uint32_t changed = 0;
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
            if (g_mdns[i].ifindex && g_mdns[i].ifindex <= MAX_L2_INTERFACES) changed |= 1u << (g_mdns[i].ifindex - 1u);
            continue;
        }
        if (out != i) g_mdns[out] = g_mdns[i];
        out++;
    }
    g_mdns_count = out;

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
            net_l4_endpoint group;
            make_ep(group4, DNS_MDNS_PORT, IP_VER4, &group);

            if (bind_socket(s, &spec, DNS_MDNS_PORT) != SOCK_OK || set_socket_option(s, SOCK_OPT_MCAST_JOIN, &group, sizeof(group)) != SOCK_OK) {
                close_socket(s);
                continue;
            }

            g_mdns[g_mdns_count].sock = s;
            g_mdns[g_mdns_count].ver = IP_VER4;
            g_mdns[g_mdns_count].ifindex = l2->ifindex;
            g_mdns[g_mdns_count].l3_id = v4->l3_id;
            g_mdns[g_mdns_count].l3_generation = v4->generation;
            memcpy(g_mdns[g_mdns_count].mcast_ip, group4, 4);
            g_mdns_count++;
            if (l2->ifindex && l2->ifindex <= MAX_L2_INTERFACES) changed |= 1u << (l2->ifindex - 1u);
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
            net_l4_endpoint group;
            make_ep(group6, DNS_MDNS_PORT, IP_VER6, &group);
            if (bind_socket(s, &spec, DNS_MDNS_PORT) != SOCK_OK || set_socket_option(s, SOCK_OPT_MCAST_JOIN, &group, sizeof(group)) != SOCK_OK) {
                close_socket(s);
                continue;
            }

            g_mdns[g_mdns_count].sock = s;
            g_mdns[g_mdns_count].ver = IP_VER6;
            g_mdns[g_mdns_count].ifindex = l2->ifindex;
            g_mdns[g_mdns_count].l3_id = v6->l3_id;
            g_mdns[g_mdns_count].l3_generation = v6->generation;
            memcpy(g_mdns[g_mdns_count].mcast_ip, group6, 16);
            g_mdns_count++;
            if (l2->ifindex && l2->ifindex <= MAX_L2_INTERFACES) changed |= 1u << (l2->ifindex-1);
        }
    }
    return changed;
}

static int dns_deamon_entry(int argc, char* argv[]) {
    (void)argc; (void)argv;

    irq_flags_t irq = irq_save_disable();
    g_dns_daemon_running = 1;
    g_dns_daemon_pending = 0;
    g_dns_dirty = 0;
    irq_restore(irq);

    uint32_t mdns_v4 = DNS_MDNS_GROUP_V4;
    uint8_t mdns_v4_addr[4];
    uint8_t mdns_v6[16];
    memcpy(mdns_v4_addr, &mdns_v4, 4);
    ipv6_make_multicast(0x02, IPV6_MCAST_MDNS, 0, mdns_v6);

    uint32_t changed = mdns_sync(mdns_v4_addr, mdns_v6);
    if (changed) mdns_reprobe(changed);

    const uint32_t tick_ms = 100;
    const uint32_t sync_ms = 1000;
    uint64_t last_socket_sync_ms = (uint32_t)get_time();
    while (mdns_has_work()) {
        irq = irq_save_disable();
        bool dirty = g_dns_dirty != 0;
        g_dns_dirty = 0;
        irq_restore(irq);

        uint32_t now_ms = (uint32_t)get_time();
        if (dirty || now_ms - last_socket_sync_ms >= sync_ms) {
            last_socket_sync_ms = now_ms;
            changed = mdns_sync(mdns_v4_addr, mdns_v6);
            if (changed) mdns_reprobe(changed);
        }

        uint8_t buf[900];
        net_l4_endpoint src;

        for (uint16_t sidx = 0; sidx < g_mdns_count; sidx++) {
            socket_handle_t s = g_mdns[sidx].sock;
            for (int i = 0; i < 64; i++) {
                int64_t r = receive_from_socket(s, buf, sizeof(buf), &src);
                if (r == SOCK_ERR_WOULDBLOCK) break;
                if (r < 0) break;
                if (!r) continue;
                mdns_rx(s, g_mdns[sidx].l3_id, g_mdns[sidx].ver, g_mdns[sidx].mcast_ip, buf, (uint32_t)r, &src);
            }
        }

        mdns_responder_tick_multi(g_mdns, g_mdns_count);
        if (!mdns_has_work()) break;
        msleep(tick_ms);
    }

    for (uint16_t i = 0; i < g_mdns_count; i++) close_socket(g_mdns[i].sock);
    g_mdns_count = 0;

    irq = irq_save_disable();
    g_dns_daemon_running = 0;
    g_dns_dirty = 0;
    irq_restore(irq);
    dns_daemon_kick();
    return 0;
}

void dns_daemon_kick(void) {
    if (!mdns_has_work()) return;

    irq_flags_t irq = irq_save_disable();
    if (g_dns_daemon_running) {
        g_dns_dirty = 1;
        irq_restore(irq);
        return;
    }

    if (g_dns_daemon_pending) {
        irq_restore(irq);
        return;
    }

    g_dns_daemon_pending = 1;
    irq_restore(irq);

    if (!create_kernel_process("dns_daemon", dns_deamon_entry, 0, 0)) {
        irq = irq_save_disable();
        g_dns_daemon_pending = 0;
        irq_restore(irq);
    }
}
