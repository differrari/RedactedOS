#include "igmp.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "net/checksums.h"
#include "networking/interface_manager.h"
#include "kernel_processes/kprocess_loader.h"
#include "math/rng.h"
#include "std/memory.h"
#include "std/string.h"
#include "syscalls/syscalls.h"

#define IGMP_TYPE_QUERY 0x11
#define IGMP_TYPE_V2_REPORT 0x16
#define IGMP_TYPE_V2_LEAVE 0x17

typedef struct __attribute__((packed)) igmp_hdr_t {
    uint8_t type;
    uint8_t max_resp_time;
    uint16_t checksum;
    uint32_t group;
} igmp_hdr_t;

typedef struct {
    uint8_t used;
    uint8_t ifindex;
    uint32_t group;
    uint32_t refresh_ms;
    uint32_t query_due_ms;
    uint8_t query_pending;
} igmp_state_t;

static volatile int igmp_daemon_running = 0;
static uint32_t igmp_uptime_ms = 0;
static rng_t igmp_rng;
static int igmp_rng_inited = 0;

#define IGMP_MAX_TRACK 64
#define IGMP_REFRESH_PERIOD_MS 60000

static igmp_state_t igmp_states[IGMP_MAX_TRACK];

static bool send_igmp(uint8_t ifindex, uint32_t dst, uint8_t type, uint32_t group) {
    uint32_t headroom = (uint32_t)sizeof(eth_hdr_t) + (uint32_t)sizeof(ipv4_hdr_t);
    netpkt_t* pkt = netpkt_alloc(sizeof(igmp_hdr_t),headroom, 0);
    if (!pkt) return false;

    igmp_hdr_t* h = (igmp_hdr_t*)netpkt_put(pkt, sizeof(igmp_hdr_t));
    if (!h) {
        netpkt_unref(pkt);
        return false;
    }

    h->type = type;
    h->max_resp_time = 0;
    h->group = bswap32(group);
    h->checksum = 0;
    h->checksum = checksum16((const uint16_t*)h, sizeof(igmp_hdr_t)/2);

    ipv4_tx_opts_t tx;
    tx.scope = IP_TX_BOUND_L2;
    tx.index = ifindex;

    ipv4_send_packet(dst, 2, pkt, &tx, 1, 0);
    return true;
}

static igmp_state_t* igmp_find_state(uint8_t ifindex, uint32_t group) {
    for (int i = 0; i < IGMP_MAX_TRACK; ++i) {
        igmp_state_t* s = &igmp_states[i];
        if (!s->used) continue;
        if (s->ifindex == ifindex &&s->group == group) return s;
    }
    return 0;
}

static igmp_state_t* igmp_get_state(uint8_t ifindex, uint32_t group) {
    igmp_state_t* s = igmp_find_state(ifindex, group);
    if (s) return s;
    for (int i = 0; i < IGMP_MAX_TRACK; ++i) {
        if (!igmp_states[i].used) {
            igmp_states[i].used = 1;
            igmp_states[i].ifindex = ifindex;
            igmp_states[i].group = group;
            igmp_states[i].refresh_ms = 0;
            igmp_states[i].query_due_ms = 0;
            igmp_states[i].query_pending = 0;
            return &igmp_states[i];
        }
    }
    return 0;
}

static int igmp_has_pending_timers(void) {
    for (int i = 0; i < IGMP_MAX_TRACK; ++i) {
        igmp_state_t* s = &igmp_states[i];
        if (!s->used) continue;
        if (s->query_pending) return 1;
        if (s->refresh_ms < IGMP_REFRESH_PERIOD_MS) return 1;
    }
    return 0;
}

static int igmp_daemon_entry(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    igmp_daemon_running = 1;

    if (!igmp_rng_inited) {
        uint64_t virt_timer;
        asm volatile ("mrs %0, cntvct_el0" : "=r"(virt_timer));
        rng_seed(&igmp_rng, virt_timer);
        igmp_rng_inited = 1;
    }

    const uint32_t tick_ms = 100;

    while (igmp_has_pending_timers()) {
        igmp_uptime_ms += tick_ms;

        for (int i = 0; i < IGMP_MAX_TRACK; ++i) {
            igmp_state_t* s = &igmp_states[i];
            if (!s->used) continue;

            l2_interface_t* l2 = l2_interface_find_by_index(s->ifindex);
            bool still_joined = false;
            if (l2) {
                for (int j = 0; j < (int)l2->ipv4_mcast_count; ++j) {
                    if (l2->ipv4_mcast[j] == s->group) {
                        still_joined = true;
                        break;
                    }
                }
            }
            if (!still_joined) {
                s->used = 0;
                continue;
            }

            s->refresh_ms+= tick_ms;
            if (s->refresh_ms>= IGMP_REFRESH_PERIOD_MS) {
                s->refresh_ms = 0;
                (void)send_igmp(s->ifindex, s->group, IGMP_TYPE_V2_REPORT, s->group);
            }

            if (s->query_pending && igmp_uptime_ms >= s->query_due_ms) {
                s->query_pending = 0;
                (void)send_igmp(s->ifindex, s->group, IGMP_TYPE_V2_REPORT, s->group);
            }
        }
        msleep(tick_ms);
    }

    igmp_daemon_running = 0;
    return 0;
}

static void igmp_daemon_kick(void) {
    if (igmp_daemon_running) return;
    if (!igmp_has_pending_timers()) return;
    create_kernel_process("igmp_daemon", igmp_daemon_entry, 0, 0);
}

bool igmp_send_join(uint8_t ifindex, uint32_t group) {
    if (!ipv4_is_multicast(group)) return false;
    igmp_state_t* s = igmp_get_state(ifindex, group);
    if (s) s->refresh_ms = 0;
    igmp_daemon_kick();
    return send_igmp(ifindex, group, IGMP_TYPE_V2_REPORT, group);
}

bool igmp_send_leave(uint8_t ifindex, uint32_t group) {
    if (!ipv4_is_multicast(group)) return false;
    igmp_state_t* s = igmp_find_state(ifindex, group);
    if (s) s->used = 0;
    igmp_daemon_kick();
    return send_igmp(ifindex, IPV4_MCAST_ALL_ROUTERS, IGMP_TYPE_V2_LEAVE, group);
}

static void schedule_report(uint8_t ifindex, uint32_t group, uint32_t max_resp_ds) {
    if (!ipv4_is_multicast(group)) return;
    igmp_state_t* s = igmp_get_state(ifindex, group);
    
    if (!s) return;
    uint32_t max_ms = (uint32_t)max_resp_ds * 100;
    if (max_ms == 0) max_ms = 100;
    uint32_t delay = rng_between32(&igmp_rng, 0, max_ms);
    uint32_t due = igmp_uptime_ms + delay;
    if (!s->query_pending || due < s->query_due_ms) {
        s->query_pending = 1;
        s->query_due_ms = due;
    }
    igmp_daemon_kick();
}

void igmp_input(uint8_t ifindex, uint32_t src, uint32_t dst, const void* l4, uint32_t l4_len) {
    if (!l4 || l4_len < sizeof(igmp_hdr_t)) return;
    const igmp_hdr_t* h = (const igmp_hdr_t*)l4;
    uint16_t saved = h->checksum;
    igmp_hdr_t tmp;
    memcpy(&tmp, h, sizeof(tmp));
    tmp.checksum = 0;
    if (checksum16((const uint16_t*)&tmp, sizeof(tmp) / 2) != saved) return;

    uint8_t type = h->type;
    uint32_t group = bswap32(h->group);

    uint32_t max_resp_ds = (uint32_t)h->max_resp_time;

    if (type != IGMP_TYPE_QUERY) return;

    if (group == 0) {
        l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
        if (!l2) return;
        for (int i = 0; i < (int)l2->ipv4_mcast_count; ++i) {
            uint32_t g = l2->ipv4_mcast[i];
            if (ipv4_is_multicast(g)) schedule_report(ifindex, g, max_resp_ds);
        }
        return;
    }

    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return;

    for (int i = 0; i < (int)l2->ipv4_mcast_count; ++i) {
        if (l2->ipv4_mcast[i] == group) {
            schedule_report(ifindex, group, max_resp_ds);
            return;
        }
    }

    (void)src;
    (void)dst;
}