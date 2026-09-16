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

#define MLDV2_RTYPE_MODE_IS_INCLUDE 1
#define MLDV2_RTYPE_MODE_IS_EXCLUDE 2
#define MLDV2_RTYPE_CHANGE_TO_INCLUDE 3
#define MLDV2_RTYPE_CHANGE_TO_EXCLUDE 4

#define MLD_V2_UNSOLICITED_INTERVAL_MS 1000
#define MLD_V1_UNSOLICITED_INTERVAL_MS 10000
#define MLD_DEFAULT_QUERY_INTERVAL_MS 125000u
#define MLD_DEFAULT_ROBUSTNESS 2

#define MLD_REPORT_JOIN 1
#define MLD_REPORT_LEAVE 2
#define MLD_MAX_TRACK 64
#define MLD_MAX_QUERY_SOURCES 32

typedef struct {
    uint8_t used;
    uint8_t ifindex;
    uint8_t group[16];
    uint32_t query_due_ms;
    uint32_t change_due_ms;
    uint8_t query_pending;
    uint8_t query_source_count;
    uint8_t change_left;
    uint8_t change_kind;
    uint8_t last_reporter;
    uint8_t query_sources[MLD_MAX_QUERY_SOURCES][16];
} mld_state_t;

static volatile int mld_daemon_running = 0;
static volatile int mld_daemon_pending = 0;
static rng_t mld_rng;
static int mld_rng_inited = 0;
static mld_state_t mld_states[MLD_MAX_TRACK];
static uint32_t mld_v1_until_ms[MAX_L2_INTERFACES];

static void mld_daemon_kick(void);

static bool mld_v1_mode(uint8_t ifindex, uint32_t now_ms) {
    if (!ifindex || ifindex > MAX_L2_INTERFACES) return false;
    uint32_t* until = &mld_v1_until_ms[ifindex - 1];
    if (!*until || (int32_t)(*until - now_ms) <= 0) *until = 0;
    return *until != 0;
}

static bool mld_dest_assigned(uint8_t ifindex, const uint8_t dst[16]) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return false;
    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (!ipv6_l3_is_ready(v6)) continue;
        if (ipv6_cmp(v6->ip, dst) == 0) return true;
    }
    for (int i = 0; i < (int)l2->ipv6_mcast_count; i++) if (ipv6_cmp(l2->ipv6_mcast[i], dst) == 0) return true;
    return false;
}

static bool mld_send_message(uint8_t ifindex, const uint8_t dst_ip[16], uint8_t* icmp, uint32_t icmp_len) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return false;
    uint8_t src_ip[16] = {0};
    uint8_t dst_mac[6];
    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (!ipv6_l3_is_ready(v6) || !ipv6_is_linklocal(v6->ip)) continue;
        ipv6_cpy(src_ip, v6->ip);
        break;
    }
    ipv6_multicast_mac(dst_ip, dst_mac);

    wr_be16(icmp+2, 0);
    wr_be16(icmp + 2, checksum16_pipv6(src_ip, dst_ip, PROTO_ICMPV6, icmp, icmp_len));
    const uint8_t hbh[8] = {PROTO_ICMPV6,0,5,2,0,0,1,0};

    uint32_t payload_len = (uint32_t)sizeof(hbh) + icmp_len;
    uint32_t total = (uint32_t)sizeof(ipv6_hdr_t) + payload_len;
    uint32_t headroom = (((uint32_t)sizeof(eth_hdr_t) + 7u) & ~7u);

    netpkt_t* pkt = netpkt_alloc(total, headroom, 0);
    if(!pkt) return false;

    void* buf = netpkt_put(pkt, total);
    if(!buf) {
        netpkt_unref(pkt);
        return false;
    }

    ipv6_hdr_t ip6;
    ip6.ver_tc_fl = bswap32((uint32_t)(6 << 28));

    ip6.payload_len = bswap16((uint16_t)payload_len);
    ip6.next_header = IPV6_NH_HOP_BY_HOP;
    ip6.hop_limit = 1;
    ipv6_cpy(ip6.src, src_ip);
    ipv6_cpy(ip6.dst, dst_ip);
    memcpy(buf, &ip6, sizeof(ip6));

    memcpy(buf + sizeof(ip6), hbh, sizeof(hbh));
    memcpy(buf + sizeof(ip6) + sizeof(hbh), icmp, icmp_len);

    return eth_send_frame_on(ifindex, ETHERTYPE_IPV6, dst_mac, pkt);
}

static bool mld_send_v1(uint8_t ifindex, const uint8_t group[16], bool done) {
    uint8_t msg[24];
    uint8_t dst[16];
    memset(msg, 0, sizeof(msg));
    msg[0] = done ? ICMPV6_MLD_DONE : ICMPV6_MLD_REPORT;
    ipv6_cpy(msg + 8, group);

    if (done) ipv6_make_multicast(2, IPV6_MCAST_ALL_ROUTERS, NULL, dst);
    else ipv6_cpy(dst, group);
    return mld_send_message(ifindex, dst, msg, sizeof(msg));
}

static bool mld_send_v2(uint8_t ifindex, const uint8_t group[16], uint8_t record_type, const uint8_t sources[][16], uint8_t source_count) {
    uint8_t msg[28 + MLD_MAX_QUERY_SOURCES*16];
    uint8_t dst[16]; 
    uint32_t msg_len = 28 + (uint32_t)source_count * 16;
    memset(msg, 0, msg_len);
    msg[0] = ICMPV6_MLDV2_REPORT;
    wr_be16(msg + 6, 1);
    msg[8] = record_type;
    wr_be16(msg + 10, source_count);
    ipv6_cpy(msg + 12, group);
    for (uint8_t i = 0; i < source_count; i++) ipv6_cpy(msg + 28 + (uint32_t)i * 16, sources[i]);
    ipv6_make_multicast(2, IPV6_MCAST_MLDV2_ROUTERS, NULL, dst);
    return mld_send_message(ifindex, dst, msg, msg_len);
}

static bool mld_send_state_change(uint8_t ifindex, const uint8_t group[16], uint8_t kind) {
    if (mld_v1_mode(ifindex, get_time())) {
        if (kind == MLD_REPORT_JOIN) return mld_send_v1(ifindex, group, false);
        return mld_send_v1(ifindex, group, true);
    }
    return mld_send_v2(ifindex, group, kind == MLD_REPORT_JOIN ? MLDV2_RTYPE_CHANGE_TO_EXCLUDE : MLDV2_RTYPE_CHANGE_TO_INCLUDE, NULL, 0);
}

static mld_state_t* mld_find_state(uint8_t ifindex, const uint8_t group[16]) {
    for(int i = 0; i < (int)N_ARR(mld_states); i++) {
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

    for(int i = 0; i < (int)N_ARR(mld_states); i++) {
        s = &mld_states[i];
        if (s->used) continue;
        memset(s, 0, sizeof(*s));
        s->used = 1;
        s->ifindex = ifindex;
        ipv6_cpy(s->group, group);
        return s;
    }
    return NULL;
}

static int mld_has_pending_timers(void) {
    for(int i = 0; i < (int)N_ARR(mld_states); i++) {
        mld_state_t* s =&mld_states[i];
        if(!s->used) continue;
        if (s->query_pending || s->change_left) return 1;
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

        for(int i = 0; i < (int)N_ARR(mld_states); i++) {
            mld_state_t* s = &mld_states[i];
            if(!s->used) continue;

            bool joined = false;
            l2_interface_t* l2 = l2_interface_find_by_index(s->ifindex);
            if (l2) {
                for (int j = 0; j < (int)l2->ipv6_mcast_count; j++) {
                    if (ipv6_cmp(l2->ipv6_mcast[j], s->group) != 0) continue;
                    joined = true;
                    break;
                }
            }
            if (!joined) {
                s->query_pending = 0;
                if (s->change_kind == MLD_REPORT_JOIN) s->change_left = 0;
            }

            if (s->query_pending && (int32_t)(now_ms - s->query_due_ms) >= 0) {
                s->query_pending = 0;
                bool v1 = mld_v1_mode(s->ifindex, now_ms);
                bool sent;
                if (v1) sent = mld_send_v1(s->ifindex, s->group, false);
                else if (s->query_source_count) sent = mld_send_v2(s->ifindex, s->group, MLDV2_RTYPE_MODE_IS_INCLUDE, s->query_sources, s->query_source_count);
                else sent = mld_send_v2(s->ifindex, s->group, MLDV2_RTYPE_MODE_IS_EXCLUDE, NULL, 0);
                s->query_source_count = 0;
                if (sent && v1) s->last_reporter = 1;
            }

            if (s->change_left && (int32_t)(now_ms - s->change_due_ms) >= 0) {
                bool sent = mld_send_state_change(s->ifindex, s->group, s->change_kind);
                if (sent && s->change_kind == MLD_REPORT_JOIN && mld_v1_mode(s->ifindex, now_ms)) s->last_reporter = 1;
                s->change_left--;
                if (s->change_left) {
                    uint32_t interval = mld_v1_mode(s->ifindex, get_time()) ? MLD_V1_UNSOLICITED_INTERVAL_MS : MLD_V2_UNSOLICITED_INTERVAL_MS;
                    s->change_due_ms = now_ms + rng_between32(&mld_rng, 1, interval + 1);
                }
            }

            if (!joined && !s->query_pending && !s->change_left) memset(s, 0, sizeof(*s));
        }

        msleep(tick_ms);
    }

    mld_daemon_running = 0;
    mld_daemon_kick();
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
    if ((group[1] & 0x0F) < 2) return true;
    uint8_t all_nodes[16];
    ipv6_make_multicast(2, IPV6_MCAST_ALL_NODES, NULL, all_nodes);
    if (ipv6_cmp(group, all_nodes) == 0) return true;

    if (!mld_rng_inited) {
        rng_init_random(&mld_rng);
        mld_rng_inited = 1;
    }

    mld_state_t* s = mld_get_state(ifindex, group);
    if (!s) return false;

    bool ok = mld_send_state_change(ifindex, group, MLD_REPORT_JOIN);
    if (ok && mld_v1_mode(ifindex, get_time())) s->last_reporter = 1;
    s->change_kind = MLD_REPORT_JOIN;
    s->change_left = MLD_DEFAULT_ROBUSTNESS - 1;
    uint32_t interval = mld_v1_mode(ifindex, get_time()) ? MLD_V1_UNSOLICITED_INTERVAL_MS : MLD_V2_UNSOLICITED_INTERVAL_MS;
    s->change_due_ms = get_time() + rng_between32(&mld_rng, 1, interval + 1);
    mld_daemon_kick();

    return ok;
}

bool mld_send_leave(uint8_t ifindex, const uint8_t group[16]) {
    if (!group || !ipv6_is_multicast(group)) return false;
    if ((group[1] & 0x0F) < 2) return true;
    uint8_t all_nodes[16];
    ipv6_make_multicast(2, IPV6_MCAST_ALL_NODES, NULL, all_nodes);
    if (ipv6_cmp(group, all_nodes) == 0) return true;

    mld_state_t* s = mld_find_state(ifindex, group);
    bool v1 = mld_v1_mode(ifindex, get_time());
    if (v1) {
        bool send_done = !s || s->last_reporter;
        if (s) memset(s, 0, sizeof(*s));
        if (!send_done) return true;
        return mld_send_state_change(ifindex, group, MLD_REPORT_LEAVE);
    }

    if (!s) s = mld_get_state(ifindex, group);
    if (!s) return false;

    bool ok = mld_send_state_change(ifindex, group, MLD_REPORT_LEAVE);
    s->query_pending = 0;
    s->change_kind = MLD_REPORT_LEAVE;
    s->change_left = MLD_DEFAULT_ROBUSTNESS - 1;
    s->change_due_ms = get_time() + rng_between32(&mld_rng, 1, MLD_V2_UNSOLICITED_INTERVAL_MS + 1);
    mld_daemon_kick();

    return ok;
}

void mld_resend_memberships(uint8_t ifindex) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2 || l2->kind == NET_IFK_LOCALHOST) return;

    for (int i = 0; i < (int)l2->ipv6_mcast_count; i++) mld_send_join(ifindex, l2->ipv6_mcast[i]);
}

static void mld_schedule_report(uint8_t ifindex, const uint8_t group[16], uint32_t max_ms, const uint8_t sources[][16], uint16_t source_count, bool v1_query) {
    if ((group[1] & 0x0F) < 2) return;
    uint8_t all_nodes[16];
    ipv6_make_multicast(2, IPV6_MCAST_ALL_NODES, NULL, all_nodes);
    if (ipv6_cmp(group, all_nodes) == 0) return;

    if (!mld_rng_inited) {
        rng_init_random(&mld_rng);
        mld_rng_inited = 1;
    }

    mld_state_t* s = mld_get_state(ifindex, group);
    if(!s) return;

    if (!s->query_pending) {
        s->query_source_count = 0;
        for (uint16_t i = 0; i < source_count; i++) {
            ipv6_cpy(s->query_sources[s->query_source_count], sources[i]);
            s->query_source_count++;
        }
    } else if (!source_count || !s->query_source_count) s->query_source_count = 0;
    else {
        uint8_t merged[MLD_MAX_QUERY_SOURCES][16];
        uint8_t merged_count = s->query_source_count;
        for (uint8_t i = 0; i < merged_count; i++) ipv6_cpy(merged[i], s->query_sources[i]);
        for (uint16_t i = 0; i < source_count; i++) {
            bool exists = false;
            for (uint8_t j = 0; j < merged_count; j++) {
                if (ipv6_cmp(merged[j], sources[i]) != 0) continue;
                exists = true;
                break;
            }
            if (exists) continue;
            if (merged_count >= N_ARR(merged)) return;
            ipv6_cpy(merged[merged_count], sources[i]);
            merged_count++;
        }
        for (uint8_t i = 0; i < merged_count; i++) ipv6_cpy(s->query_sources[i], merged[i]);
        s->query_source_count = merged_count;
    }

    uint32_t delay = max_ms ? rng_between32(&mld_rng, v1_query ? 0 : 1, max_ms +1) : 0;
    uint32_t due = get_time() + delay;

    if (!s->query_pending || (int32_t)(due - s->query_due_ms) < 0) {
        s->query_pending = 1;
        s->query_due_ms = due;
    }

    mld_daemon_kick();
}

void mld_input(uint8_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], uint8_t hop_limit, bool router_alert, netpkt_t* pkt) {
    if(!ifindex || ifindex > MAX_L2_INTERFACES || !src_ip || !dst_ip || !pkt) return;
    uint32_t l4_len = netpkt_len(pkt);
    if (l4_len < 4 || hop_limit != 1 || !router_alert || !ipv6_is_linklocal(src_ip)) return;

    uint8_t type = 0;
    if (!netpkt_copyout(pkt, 0, &type, 1)) return;

    if (type == ICMPV6_MLD_REPORT) {
        if (l4_len < 24) return;
        uint8_t msg[24];
        if (!netpkt_copyout(pkt, 0, msg, sizeof(msg))) return;
        const uint8_t* group = msg + 8;
        if (!mld_v1_mode(ifindex, get_time()) || !ipv6_is_multicast(group)) return;
        if (ipv6_cmp(dst_ip, group) != 0 && !mld_dest_assigned(ifindex, dst_ip)) return;
        mld_state_t* s = mld_find_state(ifindex, group);
        if (s) {
            s->query_pending = 0;
            s->change_left = 0;
            s->last_reporter = 0;
        }
        return;
    }

    if (type != ICMPV6_MLD_QUERY) return;
    if (l4_len != 24 && l4_len < 28) return;

    uint8_t query[28];
    if (!netpkt_copyout(pkt, 0, query, l4_len == 24 ? 24 : sizeof(query))) return;

    uint16_t max_resp_code = rd_be16(query + 4);
    uint8_t group[16];
    ipv6_cpy(group, query + 8);
    uint16_t source_count = 0;
    uint8_t sources[MLD_MAX_QUERY_SOURCES][16];
    uint32_t max_ms = 0;
    bool v1 = l4_len == 24;

    if (v1) max_ms = max_resp_code;
    else {
        source_count = rd_be16(query+26);
        uint32_t source_bytes = (uint32_t)source_count * 16;
        uint32_t expected = (uint32_t)sizeof(query) + source_bytes;
        if (expected > l4_len || source_count > N_ARR(sources)) return;
        for (uint16_t i = 0; i < source_count; i++) { 
            if (!netpkt_copyout(pkt, sizeof(query) + (uint32_t)i * 16, sources[i], 16)) return;
            if (ipv6_is_unspecified(sources[i]) || ipv6_is_multicast(sources[i])) return;
        }
        if (max_resp_code < 0x8000) max_ms = max_resp_code;
        else {
            uint32_t exp = ((uint32_t)max_resp_code >> 12) & 0x07;
            uint32_t mant = (uint32_t)max_resp_code & 0x0FFF;
            max_ms = (mant | 0x1000) << (exp + 3);
        }
    }

    bool general = ipv6_is_unspecified(group);
    if (!general && !ipv6_is_multicast(group)) return;
    if (general) {
        if (source_count) return;
        uint8_t all_nodes[16];
        ipv6_make_multicast(2, IPV6_MCAST_ALL_NODES, 0, all_nodes);
        if (ipv6_cmp(dst_ip, all_nodes) != 0 && !mld_dest_assigned(ifindex, dst_ip)) return;
    } else if (ipv6_cmp(dst_ip, group) != 0 && !mld_dest_assigned(ifindex, dst_ip)) return;

    if (v1 && general) {
        uint32_t now_ms = get_time();
        bool was_v1 = mld_v1_mode(ifindex, now_ms);
        uint64_t interval = (uint64_t)MLD_DEFAULT_ROBUSTNESS*MLD_DEFAULT_QUERY_INTERVAL_MS + max_ms;
        if (interval > 0x7FFFFFFF) interval = 0x7FFFFFFF;
        mld_v1_until_ms[ifindex - 1] = now_ms + (uint32_t)interval;
        if (!was_v1) {
            for (int i = 0; i < (int)N_ARR(mld_states); i++) {
                mld_state_t* pending = &mld_states[i];
                if (!pending->used || pending->ifindex != ifindex) continue;
                pending->query_pending = 0;
                pending->query_source_count = 0;
                pending->change_left = 0;
            }
        }
    }

    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if(!l2) return;

    if (general) {
        for(int i = 0; i < (int)l2->ipv6_mcast_count; i++) mld_schedule_report(ifindex, l2->ipv6_mcast[i], max_ms, 0, 0, v1);
        return;
    }

    for(int i = 0; i < (int)l2->ipv6_mcast_count; i++) {
        if(ipv6_cmp(l2->ipv6_mcast[i], group) != 0) continue;
        mld_schedule_report(ifindex, group, max_ms, source_count ? sources : NULL, source_count, v1);
        return;
    }
}
