#include "networking/internet_layer/mld.h"

#include "kernel_processes/kprocess_loader.h"
#include "math/rng.h"
#include "random/random.h"
#include "networking/interface_manager.h"
#include "net/checksums.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/internet_layer/icmpv6.h"
#include "networking/link_layer/eth.h"
#include "networking/link_layer/nic_types.h"
#include "std/memory.h"
#include "syscalls/syscalls.h"

#define MLDV2_RTYPE_MODE_IS_EXCLUDE 2
#define MLDV2_RTYPE_CHANGE_TO_INCLUDE 3
#define MLDV2_RTYPE_CHANGE_TO_EXCLUDE 4
#define MLD_UNSOLICITED_REPORT_INTERVAL_MS 1000u

typedef struct {
    uint8_t used;
    uint8_t ifindex;
    uint8_t group[16];
    uint32_t query_due_ms;
    uint32_t change_due_ms;
    uint8_t query_pending;
    uint8_t change_left;
    uint8_t change_type;
} mld_state_t;

static volatile int mld_daemon_running = 0;
static volatile int mld_daemon_pending = 0;
static rng_t mld_rng;
static int mld_rng_inited = 0;

#define MLD_MAX_TRACK 64

static mld_state_t mld_states[MLD_MAX_TRACK];

static bool mld_send_report(uint8_t ifindex, const uint8_t group[16], uint8_t record_type) {
    uint8_t src_ip[16] = {0};
    uint8_t dst_ip[16];
    uint8_t dst_mac[6];
    uint8_t icmp[28];

    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return false;
    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (!ipv6_l3_is_ready(v6) || !ipv6_is_linklocal(v6->ip)) continue;
        ipv6_cpy(src_ip, v6->ip);
        break;
    }

    ipv6_make_multicast(2, IPV6_MCAST_MLDV2_ROUTERS, NULL, dst_ip);
    ipv6_multicast_mac(dst_ip, dst_mac);

    memset(icmp, 0, sizeof(icmp));
    icmp[0] = ICMPV6_MLDV2_REPORT;
    icmp[6] = 0;
    icmp[7] = 1;

    icmp[8] = record_type;
    icmp[9] = 0;
    icmp[10] = 0;
    icmp[11] = 0;
    memcpy(icmp + 12, group, 16);

    uint16_t csum = checksum16_pipv6(src_ip, dst_ip, PROTO_ICMPV6, icmp, sizeof(icmp));
    icmp[2] = (uint8_t)(csum >> 8);
    icmp[3] = (uint8_t)(csum & 0xFF);

    uint8_t hbh[8];
    hbh[0] = PROTO_ICMPV6;
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

    void* ip6p = netpkt_put(pkt, (uint32_t)sizeof(ipv6_hdr_t));
    if(!ip6p) {
        netpkt_unref(pkt);
        return false;
    }

    ipv6_hdr_t ip6;
    ip6.ver_tc_fl = bswap32((uint32_t)(6 << 28));

    ip6.payload_len = bswap16((uint16_t)payload_len);
    ip6.next_header = 0;
    ip6.hop_limit = 1;
    memcpy(ip6.src, src_ip, 16);
    memcpy(ip6.dst, dst_ip, 16);
    memcpy(ip6p, &ip6, sizeof(ip6));

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
            mld_states[i].query_due_ms = 0;
            mld_states[i].change_due_ms = 0;
            mld_states[i].query_pending = 0;
            mld_states[i].change_left = 0;
            mld_states[i].change_type = 0;
            return &mld_states[i];
        }
    }

    return NULL;
}

static int mld_has_pending_timers(void) {
    for(int i = 0; i < MLD_MAX_TRACK; i++) {
        mld_state_t* s =&mld_states[i];
        if(!s->used) continue;
        if (s->query_pending || s->change_left) return 1;
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

    mld_daemon_pending = 0;
    mld_daemon_running = 1;

    if (!mld_rng_inited) {
        rng_init_random(&mld_rng);
        mld_rng_inited = 1;
    }

    const uint32_t tick_ms = 100;

    while(mld_has_pending_timers()) {
        uint32_t now_ms = get_time();

        for(int i = 0; i < MLD_MAX_TRACK; i++) {
            mld_state_t* s = &mld_states[i];
            if(!s->used) continue;

            if (s->query_pending && !mld_is_still_joined(s->ifindex, s->group)) s->query_pending = 0;
            if (s->query_pending && (int32_t)(now_ms - s->query_due_ms) >= 0) {
                s->query_pending = 0;
                (void)mld_send_report(s->ifindex, s->group, MLDV2_RTYPE_MODE_IS_EXCLUDE);
            }

            if (s->change_left && (int32_t)(now_ms - s->change_due_ms) >= 0) {
                mld_send_report(s->ifindex, s->group, s->change_type);
                s->change_left--;
                if (s->change_left) {
                    uint32_t delay = rng_between32(&mld_rng, 1, MLD_UNSOLICITED_REPORT_INTERVAL_MS + 1);
                    s->change_due_ms = now_ms + delay;
                }
            }

            if (!s->query_pending && !s->change_left && !mld_is_still_joined(s->ifindex, s->group)) memset(s, 0, sizeof(*s));
        }

        msleep(tick_ms);
    }

    mld_daemon_running = 0;
    if (!mld_daemon_pending && mld_has_pending_timers()) {
        mld_daemon_pending = 1;
        if (!create_kernel_process("mld_daemon", mld_daemon_entry, 0, 0)) mld_daemon_pending = 0;
    }
    return 0;
}

static void mld_daemon_kick(void) {
    if(mld_daemon_running || mld_daemon_pending) return;
    if(!mld_has_pending_timers()) return;
    mld_daemon_pending = 1;
    if(!create_kernel_process("mld_daemon", mld_daemon_entry, 0, 0)) mld_daemon_pending = 0; 
}

bool mld_send_join(uint8_t ifindex, const uint8_t group[16]) {
    if (!group || !ipv6_is_multicast(group)) return false;
    uint8_t all_nodes[16];
    ipv6_make_multicast(2, IPV6_MCAST_ALL_NODES, NULL, all_nodes);
    if ((group[1] & 0x0F) < 2 || ipv6_cmp(group, all_nodes) == 0) return true;

    if (!mld_rng_inited) {
        rng_init_random(&mld_rng);
        mld_rng_inited = 1;
    }

    mld_state_t* s = mld_get_state(ifindex, group);
    if (!s) return false;

    bool ok = mld_send_report(ifindex, group, MLDV2_RTYPE_CHANGE_TO_EXCLUDE);
    s->change_type = MLDV2_RTYPE_CHANGE_TO_EXCLUDE;
    s->change_left = 1;
    s->change_due_ms = get_time() + rng_between32(&mld_rng, 1, MLD_UNSOLICITED_REPORT_INTERVAL_MS + 1);
    mld_daemon_kick();

    return ok;
}

bool mld_send_leave(uint8_t ifindex, const uint8_t group[16]) {
    if (!group || !ipv6_is_multicast(group)) return false;
    uint8_t all_nodes[16];
    ipv6_make_multicast(2, IPV6_MCAST_ALL_NODES, NULL, all_nodes);
    if ((group[1] & 0x0F) < 2 || ipv6_cmp(group, all_nodes) == 0) return true;

    if (!mld_rng_inited) {
        rng_init_random(&mld_rng);
        mld_rng_inited = 1;
    }

    mld_state_t* s = mld_get_state(ifindex, group);
    if (!s) return false;

    bool ok = mld_send_report(ifindex, group, MLDV2_RTYPE_CHANGE_TO_INCLUDE);
    s->query_pending = 0;
    s->change_type = MLDV2_RTYPE_CHANGE_TO_INCLUDE;
    s->change_left = 1;
    s->change_due_ms = get_time() + rng_between32(&mld_rng, 1, MLD_UNSOLICITED_REPORT_INTERVAL_MS + 1);
    mld_daemon_kick();

    return ok;
}

void mld_resend_memberships(uint8_t ifindex) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2 || l2->kind == NET_IFK_LOCALHOST) return;

    for (int i = 0; i < (int)l2->ipv6_mcast_count; i++) mld_send_join(ifindex, l2->ipv6_mcast[i]);
}

static void schedule_report(uint8_t ifindex, const uint8_t group[16], uint16_t max_resp_ms) {
    if (!group || !ipv6_is_multicast(group)) return;
    uint8_t all_nodes[16];
    ipv6_make_multicast(2, IPV6_MCAST_ALL_NODES, NULL, all_nodes);
    if ((group[1] & 0x0F) < 2 || ipv6_cmp(group, all_nodes) == 0) return;

    if (!mld_rng_inited) {
        rng_init_random(&mld_rng);
        mld_rng_inited = 1;
    }

    mld_state_t* s = mld_get_state(ifindex, group);
    if(!s) return;

    uint32_t max_ms;
    if (max_resp_ms < 0x8000) max_ms = max_resp_ms ? (uint32_t)max_resp_ms : 1;
    else {
        uint32_t exp = ((uint32_t)max_resp_ms >> 12) & 0x07;
        uint32_t mant = (uint32_t)max_resp_ms & 0x0FFF;
        max_ms = (mant | 0x1000) << (exp + 3);
    }

    uint32_t delay = rng_between32(&mld_rng, 0, max_ms);
    uint32_t due = get_time() + delay;

    if (!s->query_pending || (int32_t)(due - s->query_due_ms) < 0) {
        s->query_pending = 1;
        s->query_due_ms = due;
    }

    mld_daemon_kick();
}

void mld_input(uint8_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], netpkt_t* pkt) {
    if(!ifindex || !src_ip || !dst_ip || !pkt) return;
    uint32_t l4_len = netpkt_len(pkt);
    if(l4_len < 8) return;

    uint8_t type = 0;
    if (!netpkt_copyout(pkt, 0, &type, 1)) return;

    if (type != ICMPV6_MLD_QUERY) return;
    if(l4_len < 24) return;
    if (!ipv6_is_linklocal(src_ip)) return;

    uint8_t query[24];
    uint8_t group[16];
    if (!netpkt_copyout(pkt, 0, query, sizeof(query))) return;
    uint16_t max_resp_ms = rd_be16(query + 4);
    memcpy(group, query + 8, sizeof(group));

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
