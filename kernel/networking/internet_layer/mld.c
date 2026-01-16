#include "networking/internet_layer/mld.h"

#include "kernel_processes/kprocess_loader.h"
#include "math/rng.h"
#include "networking/interface_manager.h"
#include "net/checksums.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/link_layer/eth.h"
#include "std/memory.h"
#include "syscalls/syscalls.h"

#define MLD_TYPE_QUERY 130
#define MLD_TYPE_REPORT_V1 131
#define MLD_TYPE_DONE_V1 132
#define MLD_TYPE_REPORT_V2 143

#define MLDV2_RTYPE_MODE_IS_INCLUDE 1
#define MLDV2_RTYPE_MODE_IS_EXCLUDE 2
#define MLDV2_RTYPE_CHANGE_TO_INCLUDE 3
#define MLDV2_RTYPE_CHANGE_TO_EXCLUDE 4
#define MLDV2_RTYPE_ALLOW_NEW_SOURCES 5
#define MLDV2_RTYPE_BLOCK_OLD_SOURCES 6

typedef struct {
    uint8_t used;
    uint8_t ifindex;
    uint8_t group[16];
    uint32_t refresh_ms;
    uint32_t query_due_ms;
    uint8_t query_pending;
} mld_state_t;

static volatile int mld_daemon_running = 0;
static uint32_t mld_uptime_ms = 0;
static rng_t mld_rng;
static int mld_rng_inited = 0;

#define MLD_MAX_TRACK 64
#define MLD_REFRESH_PERIOD_MS 60000

static mld_state_t mld_states[MLD_MAX_TRACK];

static bool mld_pick_src_ip(uint8_t ifindex, uint8_t out_src_ip[16]);
static mld_state_t* mld_find_state(uint8_t ifindex, const uint8_t group[16]);

static int mld_is_our_src(uint8_t ifindex, const uint8_t src_ip[16]) {
    uint8_t my_ip[16];
    if(!mld_pick_src_ip(ifindex, my_ip)) return 0;
    return (ipv6_cmp(my_ip, src_ip) == 0);
}

static void mld_suppress_pending(uint8_t ifindex, const uint8_t src_ip[16], const uint8_t group[16]) {
    if(mld_is_our_src(ifindex, src_ip)) return;

    mld_state_t* s = mld_find_state(ifindex, group);
    if(!s) return;
    if(!s->query_pending) return;

    s->query_pending = 0;
    s->query_due_ms = 0;
}

static bool mld_pick_src_ip(uint8_t ifindex, uint8_t out_src_ip[16]) {
    l2_interface_t *l2;
    l3_ipv6_interface_t *best;
    l3_ipv6_interface_t *v6;
    uint8_t i;

    l2 = l2_interface_find_by_index(ifindex);
    if(!l2) return false;

    best = NULL;
    for(i = 0; i < l2->ipv6_count; i++) {
        v6 = l2->l3_v6[i];
        if(!v6) continue;
        if(ipv6_is_unspecified(v6->ip)) continue;
        if(ipv6_is_linklocal(v6->ip)) {


            memcpy(out_src_ip, v6->ip, 16);
            return true;
        }
        if(!best) best = v6;
    }

    if(!best) return false;
    memcpy(out_src_ip, best->ip, 16);
    return true;
}

static bool mld_send_report(uint8_t ifindex, const uint8_t group[16], uint8_t record_type) {
    uint8_t src_ip[16];
    uint8_t dst_ip[16];
    uint8_t dst_mac[6];
    uint8_t icmp[28];

    if(!mld_pick_src_ip(ifindex, src_ip)) return false;

    ipv6_make_multicast(2, IPV6_MCAST_MLDV2_ROUTERS, NULL, dst_ip);
    ipv6_multicast_mac(dst_ip, dst_mac);

    memset(icmp, 0, sizeof(icmp));
    icmp[0] = MLD_TYPE_REPORT_V2;
    icmp[6] = 0;
    icmp[7] = 1;

    icmp[8] = record_type;
    icmp[9] = 0;
    icmp[10] = 0;
    icmp[11] = 0;
    memcpy(icmp + 12, group, 16);

    uint16_t csum = checksum16_pipv6(src_ip, dst_ip, 58, icmp, sizeof(icmp));
    icmp[2] = (uint8_t)(csum >> 8);
    icmp[3] = (uint8_t)(csum & 0xFF);

    uint8_t hbh[8];
    hbh[0] = 58;
    hbh[1] = 0;
    hbh[2] = 5;
    hbh[3] = 2;
    hbh[4] = 0;
    hbh[5] = 0;
    hbh[6] = 0;
    hbh[7] = 0;

    uint32_t payload_len = (uint32_t)sizeof(hbh) + (uint32_t)sizeof(icmp);
    uint32_t total = (uint32_t)sizeof(ipv6_hdr_t) + payload_len;
    uint32_t headroom = (((uint32_t)sizeof(eth_hdr_t) + 7u) & ~7u);

    netpkt_t* pkt = netpkt_alloc(total, headroom, 0);
    if(!pkt) return false;

    ipv6_hdr_t* ip6 = (ipv6_hdr_t*)netpkt_put(pkt, (uint32_t)sizeof(ipv6_hdr_t));
    if(!ip6) {
        netpkt_unref(pkt);
        return false;
    }

    ((uint8_t*)&ip6->ver_tc_fl)[0] = 0x60;
    ((uint8_t*)&ip6->ver_tc_fl)[1] = 0x00;
    ((uint8_t*)&ip6->ver_tc_fl)[2] = 0x00;
    ((uint8_t*)&ip6->ver_tc_fl)[3] = 0x00;

    ip6->payload_len = bswap16((uint16_t)payload_len);
    ip6->next_header = 0;
    ip6->hop_limit = 1;
    memcpy(ip6->src, src_ip, 16);
    memcpy(ip6->dst, dst_ip, 16);

    uint8_t* hb = (uint8_t*)netpkt_put(pkt, (uint32_t)sizeof(hbh));
    if(!hb) {
        netpkt_unref(pkt);
        return false;
    }
    memcpy(hb, hbh, sizeof(hbh));

    uint8_t* icmp_p = (uint8_t*)netpkt_put(pkt, (uint32_t)sizeof(icmp));
    if(!icmp_p) {
        netpkt_unref(pkt);
        return false;
    }
    memcpy(icmp_p, icmp, sizeof(icmp));

    return eth_send_frame_on(ifindex, ETHERTYPE_IPV6, dst_mac, pkt);
}

static mld_state_t* mld_find_state(uint8_t ifindex, const uint8_t group[16]) {
    for(int i = 0; i < MLD_MAX_TRACK; i++) {
        mld_state_t* s = &mld_states[i];
        if(!s->used) continue;
        if(s->ifindex != ifindex) continue;
        if(ipv6_cmp(s->group, group) == 0) return s;
    }
    return NULL;
}

static mld_state_t* mld_get_state(uint8_t ifindex, const uint8_t group[16]) {
    mld_state_t* s = mld_find_state(ifindex, group);
    if(s) return s;

    for(int i = 0; i < MLD_MAX_TRACK; i++) {
        if(!mld_states[i].used) {
            mld_states[i].used = 1;
            mld_states[i].ifindex = ifindex;
            ipv6_cpy(mld_states[i].group, group);
            mld_states[i].refresh_ms = 0;
            mld_states[i].query_due_ms = 0;
            mld_states[i].query_pending = 0;
            return &mld_states[i];
        }
    }

    return NULL;
}

static int mld_has_pending_timers(void) {
    for(int i = 0; i < MLD_MAX_TRACK; i++) {
        mld_state_t* s =&mld_states[i];
        if(!s->used) continue;
        if(s->query_pending) return 1;
        if(s->refresh_ms < MLD_REFRESH_PERIOD_MS) return 1;
    }
    return 0;
}

static int mld_is_still_joined(uint8_t ifindex, const uint8_t group[16]) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if(!l2) return 0;

    for(int i = 0; i < (int)l2->ipv6_mcast_count; i++) {
        if(ipv6_cmp(l2->ipv6_mcast[i], group) == 0) return 1;
    }

    return 0;
}

static int mld_daemon_entry(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    mld_daemon_running = 1;

    if(! mld_rng_inited) {
        uint64_t virt_timer;
        asm volatile ("mrs %0, cntvct_el0" : "=r"(virt_timer));
        rng_seed(&mld_rng, virt_timer);
        mld_rng_inited = 1;
    }

    const uint32_t tick_ms = 100;

    while(mld_has_pending_timers()) {
        mld_uptime_ms += tick_ms;

        for(int i = 0; i < MLD_MAX_TRACK; i++) {
            mld_state_t* s = &mld_states[i];
            if(!s->used) continue;

            if(!mld_is_still_joined(s->ifindex, s->group)) {
                s->used = 0;
                continue;
            }

            s->refresh_ms += tick_ms;
            if(s->refresh_ms >= MLD_REFRESH_PERIOD_MS) {
                s->refresh_ms = 0;
                (void)mld_send_report(s->ifindex, s->group, MLDV2_RTYPE_MODE_IS_EXCLUDE);
            }

            if(s->query_pending && mld_uptime_ms >= s->query_due_ms) {
                s->query_pending = 0;
                (void)mld_send_report(s->ifindex, s->group, MLDV2_RTYPE_MODE_IS_EXCLUDE);
            }
        }

        msleep(tick_ms);
    }

    mld_daemon_running = 0;
    return 0;
}

static void mld_daemon_kick(void) {
    if(mld_daemon_running) return;
    if(!mld_has_pending_timers()) return;
    create_kernel_process("mld_daemon", mld_daemon_entry, 0, 0);
}

bool mld_send_join(uint8_t ifindex, const uint8_t group[16]) {
    if(!ipv6_is_multicast(group)) return false;

    mld_state_t* s = mld_get_state(ifindex, group);
    if(s) s->refresh_ms = 0;
    mld_daemon_kick();

    return mld_send_report(ifindex, group, MLDV2_RTYPE_MODE_IS_EXCLUDE);
}

bool mld_send_leave(uint8_t ifindex, const uint8_t group[16]) {
    if(!ipv6_is_multicast(group)) return false;

    mld_state_t* s = mld_find_state(ifindex, group);
    if(s) s->used = 0;
    mld_daemon_kick();

    return mld_send_report(ifindex, group, MLDV2_RTYPE_MODE_IS_INCLUDE);
}

static void schedule_report(uint8_t ifindex, const uint8_t group[16], uint16_t max_resp_ms) {
    if(!ipv6_is_multicast(group)) return;

    if(!mld_rng_inited) {
        uint64_t virt_timer;
        asm volatile ("mrs %0, cntvct_el0" : "=r"(virt_timer));
        rng_seed(&mld_rng, virt_timer);
        mld_rng_inited = 1;
    }

    mld_state_t* s = mld_get_state(ifindex, group);
    if(!s) return;

    uint32_t max_ms = (uint32_t)max_resp_ms;
    if(max_ms == 0) max_ms = 100;

    uint32_t delay = rng_between32(&mld_rng, 0, max_ms);
    uint32_t due = mld_uptime_ms + delay;

    if(!s->query_pending || due < s->query_due_ms) {
        s->query_pending = 1;
        s->query_due_ms = due;
    }

    mld_daemon_kick();
}

void mld_input(uint8_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], const void* l4, uint32_t l4_len) {
    if(!ifindex || !src_ip || !dst_ip || !l4) return;
    if(l4_len < 8) return;

    const uint8_t* p = (const uint8_t*)l4;
    uint8_t type = p[0];

    if(type == MLD_TYPE_REPORT_V2) {
        if(l4_len < 8) return;
        uint16_t nrec = (uint16_t)((uint16_t)p[6] << 8) | (uint16_t)p[7];
        uint32_t off = 8;

        for(uint16_t i = 0; i < nrec; i++) {
            if(off + 20u > l4_len) break;

            uint8_t rtype = p[off + 0];
            uint8_t aux_words = p[off + 1];
            uint16_t nsrc = (uint16_t)((uint16_t)p[off + 2] << 8) | (uint16_t)p[off + 3];
            const uint8_t* group = p + off + 4;
            off += 20;

            uint32_t src_bytes = (uint32_t)nsrc * 16u;
            if(off + src_bytes > l4_len) break;
            off += src_bytes;

            uint32_t aux_bytes = (uint32_t)aux_words * 4u;
            if(off + aux_bytes > l4_len) break;
            off += aux_bytes;

            int interest = 0;
            if(rtype == MLDV2_RTYPE_MODE_IS_EXCLUDE || rtype == MLDV2_RTYPE_CHANGE_TO_EXCLUDE || rtype == MLDV2_RTYPE_ALLOW_NEW_SOURCES) {
                interest = 1;
            } else if((rtype == MLDV2_RTYPE_MODE_IS_INCLUDE || rtype == MLDV2_RTYPE_CHANGE_TO_INCLUDE) && nsrc) {
                interest = 1;
            }
            if(!interest) continue;

            mld_suppress_pending(ifindex, src_ip, group);
        }
        return;
    }

    if(type == MLD_TYPE_REPORT_V1) {
        if(l4_len < 24) return;
        uint8_t group[16];
        memcpy(group, p + 8, 16);
        if(ipv6_is_multicast(group)) mld_suppress_pending(ifindex, src_ip, group);
        return;
    }

    if(type != MLD_TYPE_QUERY) return;
    if(l4_len < 24) return;

    uint16_t max_resp_ms = (uint16_t)((uint16_t)p[4] << 8) | (uint16_t)p[5];

    uint8_t group[16];
    memcpy(group, p + 8, 16);

    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if(!l2) return;

    if(ipv6_is_unspecified(group)) {
        for(int i = 0; i < (int)l2->ipv6_mcast_count; i++) {
            const uint8_t* g = l2->ipv6_mcast[i];
            if(ipv6_is_multicast(g)) schedule_report(ifindex, g, max_resp_ms);
        }
        return;
    }

    for(int i = 0; i < (int)l2->ipv6_mcast_count; i++) {
        if(ipv6_cmp(l2->ipv6_mcast[i], group) == 0) {
            schedule_report(ifindex, group, max_resp_ms);
            return;
        }
    }
}
