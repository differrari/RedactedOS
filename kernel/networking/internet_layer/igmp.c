#include "igmp.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv4_route.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/link_layer/eth.h"
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
#define IGMP_TYPE_V3_REPORT 0x22

#define IGMPV3_RTYPE_MODE_IS_INCLUDE 1
#define IGMPV3_RTYPE_MODE_IS_EXCLUDE 2
#define IGMPV3_RTYPE_CHANGE_TO_INCLUDE 3
#define IGMPV3_RTYPE_CHANGE_TO_EXCLUDE 4

#define IGMPV3_ALL_ROUTERS 0xE0000016
#define IGMP_V3_UNSOLICITED_INTERVAL_MS 1000
#define IGMP_UNSOLICITED_REPORT_INTERVAL_MS 10000u
#define IGMP_DEFAULT_QUERY_INTERVAL_MS 125000u
#define IGMP_DEFAULT_ROBUSTNESS 2
#define IPV4_OPT_ROUTER_ALERT 0x94

#define IGMP_REPORT_JOIN 1
#define IGMP_REPORT_LEAVE 2
#define IGMP_MAX_QUERY_SOURCES 32

typedef struct __attribute__((packed)) igmp_hdr_t {
    uint8_t type;
    uint8_t max_resp_time;
    uint16_t checksum;
    uint32_t group;
} igmp_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t reserved1;
    uint16_t checksum;
    uint16_t reserved2;
    uint16_t num_records;
} igmpv3_report_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t record_type;
    uint8_t aux_len;
    uint16_t num_sources;
    uint32_t group;
} igmpv3_record_t;

typedef struct {
    uint8_t used;
    uint8_t ifindex;
    uint32_t group;
    uint32_t query_due_ms;
    uint32_t unsolicited_due_ms;
    uint8_t query_pending;
    uint8_t query_source_count;
    uint8_t unsolicited_left;
    uint8_t unsolicited_kind;
    uint8_t last_reporter;
    uint32_t query_sources[IGMP_MAX_QUERY_SOURCES];
} igmp_state_t;

typedef struct {
    uint32_t v1_until_ms;
    uint32_t v2_until_ms;
} igmp_if_state_t;

static volatile int igmp_daemon_running = 0;
static volatile int igmp_daemon_pending = 0;
static rng_t igmp_rng;
static int igmp_rng_inited = 0;

#define IGMP_MAX_TRACK 64

static igmp_state_t igmp_states[IGMP_MAX_TRACK];
static igmp_if_state_t igmp_if_states[MAX_L2_INTERFACES];

static void igmp_daemon_kick(void);
static uint8_t igmp_compat_mode(uint8_t ifindex, uint32_t now_ms) {
    if (!ifindex || ifindex > MAX_L2_INTERFACES) return 3;
    igmp_if_state_t* s = &igmp_if_states[ifindex - 1];
    if (s->v1_until_ms && (int32_t)(s->v1_until_ms - now_ms) <= 0) s->v1_until_ms = 0;
    if (s->v2_until_ms && (int32_t)(s->v2_until_ms - now_ms) <= 0) s->v2_until_ms = 0;
    if (s->v1_until_ms) return 1;
    if (s->v2_until_ms) return 2;
    return 3;
}

static bool igmp_send_message(uint8_t ifindex, uint32_t dst, const void* msg, uint32_t msg_len) {
    if (!msg || !msg_len) return false;

    ip_tx_opts_t tx = {.target = {.ifindex = ifindex}, .scope = IP_TX_BOUND_L2};
    ipv4_tx_plan_t plan;
    if (!ipv4_build_tx_plan(dst, &tx, &plan)) return false;

    l3_ipv4_interface_t* l3 = l3_ipv4_find_by_id(plan.l3_id);
    if (!ipv4_l3_is_ready(l3)) return false;

    const uint32_t ip_header_len = sizeof(ipv4_hdr_t) + 4;
    uint32_t total_len = ip_header_len + msg_len;
    if (total_len > UINT16_MAX || l3_ipv4_effective_mtu(l3) < total_len) return false;
    netpkt_t* pkt = netpkt_alloc(total_len, sizeof(eth_hdr_t), 0);
    if (!pkt) return false;

    uint8_t* igmp = (uint8_t*)netpkt_put(pkt, total_len);
    if (!igmp) {
        netpkt_unref(pkt);
        return false;
    }

    ipv4_hdr_t ip;
    ip.version_ihl = (uint8_t)((IP_VER4 << 4) | (IP_IHL_NOOPTS + 1));
    ip.dscp_ecn = 0xC0;
    ip.total_length = bswap16((uint16_t)total_len);
    ip.identification = 0;
    ip.flags_frag_offset = 0;
    ip.ttl = 1;
    ip.protocol = PROTO_IGMP;
    ip.header_checksum = 0;
    ip.src_ip = bswap32(plan.src_ip);
    ip.dst_ip = bswap32(dst);
    memcpy(igmp, &ip, sizeof(ip));

    const uint32_t opt_off = sizeof(ipv4_hdr_t);
    igmp[opt_off] = IPV4_OPT_ROUTER_ALERT;
    igmp[opt_off+1] = 4;
    igmp[opt_off+2] = 0;
    igmp[opt_off+3] = 0;
    ip.header_checksum = bswap16(checksum16(igmp, ip_header_len));
    memcpy(igmp, &ip, sizeof(ip));
    memcpy(igmp + ip_header_len, msg, msg_len);

    uint8_t dst_mac[MAC_ADDR_LEN];
    ipv4_mcast_to_mac(dst, dst_mac);
    return eth_send_frame_on(ifindex, ETHERTYPE_IPV4, dst_mac, pkt);
}

static bool igmp_send_v12(uint8_t ifindex, uint32_t dst, uint8_t type, uint32_t group) {
    igmp_hdr_t msg;
    msg.type = type;
    msg.max_resp_time = 0;
    msg.checksum = 0;
    msg.group = bswap32(group);
    msg.checksum = bswap16(checksum16(&msg, sizeof(msg)));
    return igmp_send_message(ifindex, dst, &msg, sizeof(msg));
}

static bool igmp_send_v3(uint8_t ifindex, uint32_t group, uint8_t record_type, const uint32_t* sources, uint8_t source_count) {
    igmpv3_report_hdr_t hdr = {0};
    igmpv3_record_t record = {0};
    uint8_t msg[sizeof(hdr) + sizeof(record) + IGMP_MAX_QUERY_SOURCES * sizeof(uint32_t)];
    uint32_t msg_len = sizeof(hdr) + sizeof(record) + source_count * sizeof(uint32_t);

    hdr.type = IGMP_TYPE_V3_REPORT;
    hdr.num_records = bswap16(1);
    record.record_type = record_type;
    record.num_sources = bswap16(source_count);
    record.group = bswap32(group);

    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), &record, sizeof(record));
    for (uint8_t i = 0; i < source_count; i++) wr_be32(msg + sizeof(hdr) + sizeof(record) + i * sizeof(uint32_t), sources[i]);
    hdr.checksum = bswap16(checksum16(msg, msg_len));
    memcpy(msg, &hdr, sizeof(hdr));
    return igmp_send_message(ifindex, IGMPV3_ALL_ROUTERS, msg, msg_len);
}

static bool igmp_send_state_change(uint8_t ifindex, uint32_t group, uint8_t kind) {
    uint8_t mode = igmp_compat_mode(ifindex, get_time());
    if (kind == IGMP_REPORT_JOIN) {
        if (mode == 1) return igmp_send_v12(ifindex, group, IGMP_TYPE_V1_REPORT, group);
        if (mode == 2) return igmp_send_v12(ifindex, group, IGMP_TYPE_V2_REPORT, group);
        return igmp_send_v3(ifindex, group, IGMPV3_RTYPE_CHANGE_TO_EXCLUDE, 0, 0);
    }

    if (mode == 1) return true;
    if (mode == 2) return igmp_send_v12(ifindex, IPV4_MCAST_ALL_ROUTERS, IGMP_TYPE_V2_LEAVE, group);
    return igmp_send_v3(ifindex, group, IGMPV3_RTYPE_CHANGE_TO_INCLUDE, 0, 0);
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
            igmp_states[i].query_source_count = 0;
            igmp_states[i].unsolicited_left = 0;
            igmp_states[i].unsolicited_kind = 0;
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
                s->query_pending = 0;
                if (s->unsolicited_kind == IGMP_REPORT_JOIN) s->unsolicited_left = 0;
                //continue;
            }

            if (s->query_pending && (int32_t)(now_ms - s->query_due_ms) >= 0) {
                s->query_pending = 0;
                uint8_t mode = igmp_compat_mode(s->ifindex, now_ms);
                bool sent;
                if (mode == 1) sent = igmp_send_v12(s->ifindex, s->group, IGMP_TYPE_V1_REPORT, s->group);
                else if (mode == 2) sent = igmp_send_v12(s->ifindex, s->group, IGMP_TYPE_V2_REPORT, s->group);
                else if (s->query_source_count) sent = igmp_send_v3(s->ifindex, s->group, IGMPV3_RTYPE_MODE_IS_INCLUDE, s->query_sources, s->query_source_count);
                else sent = igmp_send_v3(s->ifindex, s->group, IGMPV3_RTYPE_MODE_IS_EXCLUDE, 0, 0);
                s->query_source_count = 0;
                if (sent && mode != 3) s->last_reporter = 1;
            }

            if (s->unsolicited_left && (int32_t)(now_ms - s->unsolicited_due_ms) >= 0) {
                bool sent = igmp_send_state_change(s->ifindex, s->group, s->unsolicited_kind);
                uint8_t mode = igmp_compat_mode(s->ifindex, now_ms);
                if (sent && s->unsolicited_kind == IGMP_REPORT_JOIN && mode != 3) s->last_reporter = 1;
                s->unsolicited_left--;
                if (s->unsolicited_left) {
                    uint32_t interval = mode == 3 ? IGMP_V3_UNSOLICITED_INTERVAL_MS : IGMP_UNSOLICITED_REPORT_INTERVAL_MS;
                    s->unsolicited_due_ms = now_ms + rng_between32(&igmp_rng, 1, interval+1);
                }
            }

            if (!still_joined && !s->query_pending && !s->unsolicited_left) memset(s, 0, sizeof(*s));
        }
        msleep(tick_ms);
    }

    igmp_daemon_running = 0;
    igmp_daemon_kick();
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

    bool ok = igmp_send_state_change(ifindex, group, IGMP_REPORT_JOIN);
    uint8_t mode = igmp_compat_mode(ifindex, get_time());
    if (ok && mode != 3) s->last_reporter = 1;
    s->unsolicited_kind = IGMP_REPORT_JOIN;
    s->unsolicited_left = IGMP_DEFAULT_ROBUSTNESS - 1;
    uint32_t interval = mode == 3 ? IGMP_V3_UNSOLICITED_INTERVAL_MS : IGMP_UNSOLICITED_REPORT_INTERVAL_MS;
    s->unsolicited_due_ms = get_time() + rng_between32(&igmp_rng, 1, interval + 1);
    igmp_daemon_kick();
    return ok;
}

bool igmp_send_leave(uint8_t ifindex, uint32_t group) {
    if (!ipv4_is_multicast(group)) return false;
    if (group == IPV4_MCAST_ALL_HOSTS) return true;
    igmp_state_t* s = igmp_find_state(ifindex, group);
    uint8_t mode = igmp_compat_mode(ifindex, get_time());
    if (mode == 1) {
        if (s) memset(s, 0, sizeof(*s));
        return true;
    }
    if (mode == 2) {
        bool send_leave = !s || s->last_reporter;
        if (s) memset(s, 0, sizeof(*s));
        if (!send_leave) return true;
        return igmp_send_state_change(ifindex, group, IGMP_REPORT_LEAVE);
    }

    if (!s) s = igmp_get_state(ifindex, group);
    if (!s) return false;
    bool ok = igmp_send_state_change(ifindex, group, IGMP_REPORT_LEAVE);
    s->query_pending = 0;
    s->unsolicited_kind = IGMP_REPORT_LEAVE;
    s->unsolicited_left = IGMP_DEFAULT_ROBUSTNESS - 1;
    s->unsolicited_due_ms = get_time() + rng_between32(&igmp_rng, 1, IGMP_V3_UNSOLICITED_INTERVAL_MS + 1);
    igmp_daemon_kick();
    return ok;
}

static bool igmp_dest_assigned(uint8_t ifindex, uint32_t dst) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return false;
    for (int i = 0; i < MAX_IPV4_PER_INTERFACE; i++) {
        l3_ipv4_interface_t* v4 = l2->l3_v4[i];
        if (!ipv4_l3_is_active(v4)) continue;
        if (v4->ip == dst) return 1;
    }
    for (int i = 0; i < (int)l2->ipv4_mcast_count; i++) if (l2->ipv4_mcast[i] == dst) return true;
    return false;
}

static void schedule_report(uint8_t ifindex, uint32_t group, uint32_t max_ms, const uint32_t* sources, uint16_t source_count) {
    if (!ipv4_is_multicast(group) || group == IPV4_MCAST_ALL_HOSTS) return;
    if (source_count > IGMP_MAX_QUERY_SOURCES) return;
    if (!igmp_rng_inited) {
        rng_init_random(&igmp_rng);
        igmp_rng_inited = 1;
    }

    igmp_state_t* s = igmp_get_state(ifindex, group);
    if (!s) return;

    if (!s->query_pending) {
        s->query_source_count = 0;
        for (uint16_t i = 0; i < source_count; i++) s->query_sources[s->query_source_count++] = sources[i];
    } else if (!source_count || !s->query_source_count) s->query_source_count = 0;
    else {
        uint32_t merged[IGMP_MAX_QUERY_SOURCES];
        uint8_t merged_count = s->query_source_count;
        for (uint8_t i = 0; i < merged_count; i++) merged[i] = s->query_sources[i];
        for (uint16_t i = 0; i < source_count; i++) {
            bool exists = false;
            for (uint8_t j = 0; j < merged_count; j++) {
                if (merged[j] != sources[i]) continue;
                exists = true;
                break;
            }
            if (exists) continue;
            if (merged_count >= IGMP_MAX_QUERY_SOURCES) return;
            merged[merged_count++] = sources[i];
        }
        for (uint8_t i = 0; i < merged_count; i++) s->query_sources[i] = merged[i];
        s->query_source_count = merged_count;
    }
    uint32_t delay = max_ms ? rng_between32(&igmp_rng, 0, max_ms +1) : 0;
    uint32_t now_ms = get_time();
    uint32_t due = now_ms + delay;
    if (!s->query_pending || (int32_t)(due - s->query_due_ms) < 0) {
        s->query_pending = 1;
        s->query_due_ms = due;
    }
    igmp_daemon_kick();
}

void igmp_input(uint8_t ifindex, uint32_t src, uint32_t dst, const uint8_t* ip_header, uint8_t ip_header_len, netpkt_t* pkt) {
    if (!pkt) return;
    uint32_t l4_len = netpkt_len(pkt);
    if (l4_len < sizeof(igmp_hdr_t)) {
        netpkt_unref(pkt);
        return;
    }
    const uint8_t* p = (const uint8_t*)netpkt_data(pkt);
    uint8_t hdr[12];
    if (!p || !netpkt_copyout(pkt, 0, hdr, sizeof(igmp_hdr_t))) {
        netpkt_unref(pkt);
        return;
    }
    if (checksum16(p, l4_len) != 0) {
        netpkt_unref(pkt);
        return;
    }

    socket_raw_input_v4(PROTO_IGMP, ifindex, src, dst, pkt);

    ipv4_hdr_t ip = {0};
    if (ip_header && ip_header_len >= sizeof(ip)) memcpy(&ip, ip_header, sizeof(ip));

    uint8_t type = hdr[0];
    uint32_t group = rd_be32(hdr + 4);

    if (type == IGMP_TYPE_V1_REPORT || type == IGMP_TYPE_V2_REPORT) {
        if (ip.ttl == 1 && igmp_compat_mode(ifindex, get_time()) != 3) { 
            igmp_state_t* s = igmp_find_state(ifindex, group);
            if (s) {
                s->query_pending = 0;
                s->unsolicited_left = 0;
                s->last_reporter = 0;
            }
        }
        netpkt_unref(pkt);
        return;
    }

    if (type != IGMP_TYPE_QUERY || ip.ttl != 1) {
        netpkt_unref(pkt);
        return;
    }

    uint8_t version;
    uint32_t max_ms;
    uint16_t source_count = 0;
    uint32_t sources[IGMP_MAX_QUERY_SOURCES];
    if (l4_len == sizeof(igmp_hdr_t)) {
        if (hdr[1] == 0) {
            version = 1;
            max_ms = 10000;
        } else {
            version = 2;
            max_ms = hdr[1] * 100;
        }
    } else if (l4_len >= sizeof(hdr)) {
        version = 3;
        if (!netpkt_copyout(pkt, 0, hdr, sizeof(hdr))) {
            netpkt_unref(pkt);
            return;
        }
        source_count = rd_be16(hdr + 10);
        uint32_t expected = sizeof(hdr) + source_count * 4;
        if (expected != l4_len || source_count > IGMP_MAX_QUERY_SOURCES) {
            netpkt_unref(pkt);
            return;
        }
        for (uint16_t i = 0; i < source_count; i++) {
            uint8_t raw[4];
            if (!netpkt_copyout(pkt, sizeof(hdr) + i * 4, raw, sizeof(raw))) {
                netpkt_unref(pkt);
                return;
            }
            sources[i] = rd_be32(raw);
        }
        uint8_t code = hdr[1];
        if (code < 128) max_ms = code * 100;
        else {
            uint32_t exp = (code >> 4) & 0x07;
            uint32_t mant = code & 0x0F;
            max_ms = ((mant | 0x10) << (exp + 3)) * 100;
        }
    } else {
        netpkt_unref(pkt);
        return;
    }

    if (version > 1) {
        bool router_alert = false;
        if (ip_header && ip_header_len > sizeof(ipv4_hdr_t)) {
            uint32_t pos = sizeof(ipv4_hdr_t);
            while (pos < ip_header_len) {
                uint8_t opt = ip_header[pos];
                if (opt == 0) break;
                if (opt == 1) {
                    pos++;
                    continue;
                }
                if (ip_header_len - pos < 2) break;

                uint8_t len = ip_header[pos+1];
                if (len < 2 || len > ip_header_len - pos) break;
                if (opt == IPV4_OPT_ROUTER_ALERT && len == 4 && !ip_header[pos + 2] && !ip_header[pos + 3]) {
                    router_alert = true;
                    break;
                }
                pos += len;
            }
        }
        if (!router_alert) {
            netpkt_unref(pkt);
            return;
        }
    }

    if (group && !ipv4_is_multicast(group)) {
        netpkt_unref(pkt);
        return;
    }
    if (!group) {
        if (source_count || (dst != IPV4_MCAST_ALL_HOSTS && !igmp_dest_assigned(ifindex, dst))) {
            netpkt_unref(pkt);
            return;
        }
        if (version < 3 && ifindex && ifindex <= MAX_L2_INTERFACES) {
            igmp_if_state_t* s = &igmp_if_states[ifindex-1];
            uint32_t now_ms = get_time();
            uint8_t old_mode = igmp_compat_mode(ifindex, now_ms);
            uint32_t until = now_ms + IGMP_DEFAULT_ROBUSTNESS * IGMP_DEFAULT_QUERY_INTERVAL_MS + max_ms;

            if (version == 1) s->v1_until_ms = until;
            else if (version == 2) s->v2_until_ms = until;
            if (igmp_compat_mode(ifindex, now_ms) != old_mode) { 
                for (int i = 0; i < (int)N_ARR(igmp_states); i++) {
                    igmp_state_t* state = &igmp_states[i];
                    if (!state->used || state->ifindex != ifindex) continue;
                    state->query_pending = 0;
                    state->query_source_count = 0;
                    state->unsolicited_left = 0;
                }
            }
        }
    } else if (dst != group && !igmp_dest_assigned(ifindex, dst)) {
        netpkt_unref(pkt);
        return;
    }

    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) {
        netpkt_unref(pkt);
        return;
    }

    if (!group) {

        for (int i = 0; i < (int)l2->ipv4_mcast_count; i++) schedule_report(ifindex, l2->ipv4_mcast[i], max_ms, 0, 0);
    } else {
        for (int i = 0; i < (int)l2->ipv4_mcast_count; i++) {
            if (l2->ipv4_mcast[i] != group) continue;
            schedule_report(ifindex, group, max_ms, source_count ? sources : 0, source_count);
            break;
        }
    }

    netpkt_unref(pkt);
}