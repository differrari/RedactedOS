#include "igmp.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "net/checksums.h"
#include "networking/interface_manager.h"
#include "networking/transport_layer/csocket_raw.h"
#include "kernel_processes/kprocess_loader.h"
#include "math/rng.h"
#include "random/random.h"
#include "std/memory.h"
#include "std/string.h"
#include "syscalls/syscalls.h"

#define IGMP_TYPE_QUERY 0x11
#define IGMP_TYPE_V1_REPORT 0x12
#define IGMP_TYPE_V2_REPORT 0x16
#define IGMP_TYPE_V2_LEAVE 0x17
#define IGMP_UNSOLICITED_REPORT_INTERVAL_MS 10000u

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
    uint32_t query_due_ms;
    uint32_t unsolicited_due_ms;
    uint8_t query_pending;
    uint8_t unsolicited_left;
    uint8_t last_reporter;
} igmp_state_t;

static volatile int igmp_daemon_running = 0;
static volatile int igmp_daemon_pending = 0;
static rng_t igmp_rng;
static int igmp_rng_inited = 0;

#define IGMP_MAX_TRACK 64

static igmp_state_t igmp_states[IGMP_MAX_TRACK];

static bool igmp_send_packet(uint8_t ifindex, uint32_t dst, uint8_t type, uint32_t group) {
    uint32_t headroom = (uint32_t)sizeof(eth_hdr_t) + (uint32_t)sizeof(ipv4_hdr_t);
    netpkt_t* pkt = netpkt_alloc(sizeof(igmp_hdr_t), headroom, 0);
    if (!pkt) return false;

    igmp_hdr_t* igmp = (igmp_hdr_t*)netpkt_put(pkt, sizeof(igmp_hdr_t));
    if (!igmp) {
        netpkt_unref(pkt);
        return false;
    }

    igmp->type = type;
    igmp->max_resp_time = 0;
    igmp->group = bswap32(group);
    igmp->checksum = 0;
    igmp->checksum = bswap16(checksum16(igmp, sizeof(*igmp)));

    ip_tx_opts_t tx;
    tx.scope = IP_TX_BOUND_L2;
    tx.target.ifindex = ifindex;

    return ipv4_send_packet(dst, PROTO_IGMP, pkt, &tx, 1, 0);
}

static igmp_state_t* igmp_find_state(uint8_t ifindex, uint32_t group) {
    for (int i = 0; i < (int)N_ARR(igmp_states); i++) {
        igmp_state_t* s = &igmp_states[i];
        if (!s->used) continue;
        if (s->ifindex == ifindex &&s->group == group) return s;
    }
    return 0;
}

static igmp_state_t* igmp_get_state(uint8_t ifindex, uint32_t group) {
    igmp_state_t* s = igmp_find_state(ifindex, group);
    if (s) return s;
    for (int i = 0; i < (int)N_ARR(igmp_states); i++) {
        if (!igmp_states[i].used) {
            igmp_states[i].used = 1;
            igmp_states[i].ifindex = ifindex;
            igmp_states[i].group = group;
            igmp_states[i].query_due_ms = 0;
            igmp_states[i].unsolicited_due_ms = 0;
            igmp_states[i].query_pending = 0;
            igmp_states[i].unsolicited_left = 0;
            igmp_states[i].last_reporter = 0;
            return &igmp_states[i];
        }
    }
    return 0;
}

static int igmp_has_pending_timers(void) {
    for (int i = 0; i < (int)N_ARR(igmp_states); i++) {
        igmp_state_t* s = &igmp_states[i];
        if (!s->used) continue;
        if (s->query_pending || s->unsolicited_left) return 1;
    }
    return 0;
}

static int igmp_daemon_entry(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    igmp_daemon_pending = 0;
    igmp_daemon_running = 1;

    if (!igmp_rng_inited) {
        rng_init_random(&igmp_rng);
        igmp_rng_inited = 1;
    }

    const uint32_t tick_ms = 100;

    while (igmp_has_pending_timers()) {
        uint32_t now_ms = get_time();

        for (int i = 0; i < (int)N_ARR(igmp_states); i++) {
            igmp_state_t* s = &igmp_states[i];
            if (!s->used) continue;

            l2_interface_t* l2 = l2_interface_find_by_index(s->ifindex);
            bool still_joined = false;
            if (l2) {
                for (int j = 0; j < (int)l2->ipv4_mcast_count; j++) {
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

            if (s->query_pending && (int32_t)(now_ms - s->query_due_ms) >= 0) {
                s->query_pending = 0;
                if (igmp_send_packet(s->ifindex, s->group, IGMP_TYPE_V2_REPORT, s->group)) s->last_reporter = 1;
            }

            if (s->unsolicited_left && (int32_t)(now_ms - s->unsolicited_due_ms) >= 0) {
                if (igmp_send_packet(s->ifindex, s->group, IGMP_TYPE_V2_REPORT, s->group)) s->last_reporter = 1;
                s->unsolicited_left--;
                if (s->unsolicited_left) {
                    uint32_t delay = rng_between32(&igmp_rng, 1u, IGMP_UNSOLICITED_REPORT_INTERVAL_MS + 1u);
                    s->unsolicited_due_ms = now_ms + delay;
                }
            }
        }
        msleep(tick_ms);
    }

    igmp_daemon_running = 0;
    if (!igmp_daemon_pending && igmp_has_pending_timers()) {
        igmp_daemon_pending = 1;
        if (!create_kernel_process("igmp_daemon", igmp_daemon_entry, 0, 0)) igmp_daemon_pending = 0;
    }
    return 0;
}

static void igmp_daemon_kick(void) {
    if (igmp_daemon_running || igmp_daemon_pending) return;
    if (!igmp_has_pending_timers()) return;
    igmp_daemon_pending = 1;
    if (!create_kernel_process("igmp_daemon", igmp_daemon_entry, 0, 0)) igmp_daemon_pending = 0;
}

bool igmp_send_join(uint8_t ifindex, uint32_t group) {
    if (!ipv4_is_multicast(group)) return false;
    if (group == IPV4_MCAST_ALL_HOSTS) return true;

    if (!igmp_rng_inited) {
        rng_init_random(&igmp_rng);
        igmp_rng_inited = 1;
    }

    igmp_state_t* s = igmp_get_state(ifindex, group);
    if (!s) return false;

    bool ok = igmp_send_packet(ifindex, group, IGMP_TYPE_V2_REPORT, group);
    if (ok) s->last_reporter = 1;
    s->unsolicited_left = 1;
    s->unsolicited_due_ms = get_time() + rng_between32(&igmp_rng, 1u, IGMP_UNSOLICITED_REPORT_INTERVAL_MS + 1u);
    igmp_daemon_kick();
    return ok;
}

bool igmp_send_leave(uint8_t ifindex, uint32_t group) {
    if (!ipv4_is_multicast(group)) return false;
    if (group == IPV4_MCAST_ALL_HOSTS) return true;
    igmp_state_t* s = igmp_find_state(ifindex, group);
    bool send_leave = !s || s->last_reporter;
    if (s) memset(s, 0, sizeof(*s));
    if (!send_leave) return true;
    return igmp_send_packet(ifindex, IPV4_MCAST_ALL_ROUTERS, IGMP_TYPE_V2_LEAVE, group);
}

static void schedule_report(uint8_t ifindex, uint32_t group, uint32_t max_resp_ds) {
    if (!ipv4_is_multicast(group) || group == IPV4_MCAST_ALL_HOSTS) return;
    igmp_state_t* s = igmp_get_state(ifindex, group);
    if (!s) return;

    uint8_t code = (uint8_t)max_resp_ds;
    uint32_t max_ms = 10000;
    if (code && code < 128) max_ms = (uint32_t)code * 100;
    else if (code >= 128) {
        uint32_t exp = ((uint32_t)code >> 4) & 0x07;
        uint32_t mant = (uint32_t)code & 0x0F;
        max_ms = ((mant | 0x10) << (exp + 3)) * 100;
    }
    uint32_t delay = rng_between32(&igmp_rng, 0, max_ms);
    uint32_t now_ms = get_time();
    uint32_t due = now_ms + delay;
    if (!s->query_pending || (int32_t)(due - s->query_due_ms) < 0) {
        s->query_pending = 1;
        s->query_due_ms = due;
    }
    igmp_daemon_kick();
}

void igmp_input(uint8_t ifindex, uint32_t src, uint32_t dst, netpkt_t* pkt) {
    if (!pkt) return;
    uint32_t l4_len = netpkt_len(pkt);
    if (l4_len < sizeof(igmp_hdr_t)) {
        netpkt_unref(pkt);
        return;
    }
    const uint8_t* p = (const uint8_t*)netpkt_data(pkt);
    uint8_t hdr[sizeof(igmp_hdr_t)];
    if (!netpkt_copyout(pkt, 0, hdr, sizeof(hdr))) {
        netpkt_unref(pkt);
        return;
    }
    if (checksum16(p, l4_len) != 0) {
        netpkt_unref(pkt);
        return;
    }

    socket_raw_input_v4(PROTO_IGMP, ifindex, src, dst, pkt);

    uint8_t type = hdr[0];
    uint32_t group = rd_be32(hdr + 4);

    uint32_t max_resp_ds = (uint32_t)hdr[1];

    if (type == IGMP_TYPE_V1_REPORT || type == IGMP_TYPE_V2_REPORT) {
        igmp_state_t* s = igmp_find_state(ifindex, group);
        if (s) {
            s->query_pending = 0;
            s->unsolicited_left = 0;
            s->last_reporter = 0;
        }
        netpkt_unref(pkt);
        return;
    }

    if (type != IGMP_TYPE_QUERY) {
        netpkt_unref(pkt);
        return;
    }

    if (group == 0) {
        l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
        if (!l2) {
            netpkt_unref(pkt);
            return;
        }
        for (int i = 0; i < (int)l2->ipv4_mcast_count; i++) {
            uint32_t g = l2->ipv4_mcast[i];
            if (ipv4_is_multicast(g)) schedule_report(ifindex, g, max_resp_ds);
        }
        netpkt_unref(pkt);
        return;
    }

    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) {
        netpkt_unref(pkt);
        return;
    }

    for (int i = 0; i < (int)l2->ipv4_mcast_count; i++) {
        if (l2->ipv4_mcast[i] == group) {
            schedule_report(ifindex, group, max_resp_ds);
            netpkt_unref(pkt);
            return;
        }
    }

    (void)src;
    (void)dst;
    netpkt_unref(pkt);
}