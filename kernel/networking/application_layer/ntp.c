#include "ntp.h"
#include "exceptions/timer.h"
#include "std/memory.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "math/math.h"
#include "networking/transport_layer/csocket.h"
#include "networking/transport_layer/trans_utils.h"

#define NTP_PORT 123
#define NTP_VN 4
#define NTP_MODE_CLIENT 3
#define NTP_MODE_SERVER 4
#define NTP_UNIX_EPOCH_DELTA 2208988800UL

#define NTP_STEP_US 128000LL
#define NTP_FREQ_MAX_PPM 500
#define NTP_FREQ_MIN_INTERVAL_US 32000000ULL
#define NTP_PHI_PPM 15

#define NTP_REFID_DENY 0x44454E59u //DENY
#define NTP_REFID_RATE 0x52415445u
#define NTP_REFID_RSTR 0x52535452u //RSTR

#define NTP_RATE_MIN_SEC 64
#define NTP_RATE_MAX_SHIFT 7
#define NTP_MAX_DISP_US 16000000ULL
#define NTP_MAX_CANDIDATES 16
#define NTP_MAX_RULES MAX_L2_INTERFACES * NTP_MAX_CANDIDATES
#define NTP_CLOCK_GUARD_US 86400ULL * 1000000ULL
#define NTP_RECOVERY_FRESH_US 120 * 1000000ULL
#define NTP_RECOVERY_AGREEMENT_US 2000000ULL
#define NTP_RECOVERY_MAX_RTT_US 1000000ULL
#define NTP_RECOVERY_MAX_DISTANCE_US 1000000ULL

typedef struct {
    int64_t offset_us;
    uint64_t delay_us;
    uint64_t dispersion_us;
    uint64_t mono_time_us;
    uint64_t server_time_us;
    uint64_t root_delay_us;
    uint64_t root_dispersion_us;
} ntp_sample_t;

typedef struct __attribute__((packed)) {
    uint8_t li_vn_mode;
    uint8_t stratum;
    uint8_t poll;
    int8_t precision;
    uint32_t rootDelay;
    uint32_t rootDispersion;
    uint32_t refId;
    uint64_t refTs;
    uint64_t origTs;
    uint64_t recvTs;
    uint64_t txTs;
} ntp_packet_t;

typedef struct {
    net_l4_endpoint endpoint;
    l3_id_t l3_id;
    uint32_t l3_epoch;
} ntp_server_t;

typedef struct {
    ntp_server_t server;
    uint64_t last_sample_mono_us;
    uint64_t last_server_time_us;
    int64_t last_filt_offset_us;
    ntp_sample_t filt[NTP_FILTER_N];
    uint8_t count;
    uint64_t best_delay_us;
    uint64_t jitter_us;
    uint64_t root_distance_us;
    int64_t best_offset_us;
    uint64_t best_sample_mono_us;
    uint64_t best_server_time_us;
    uint64_t suspect_mono_us;
    uint64_t suspect_server_us;
    uint64_t suspect_distance_us;
    uint8_t suspect_votes;
    bool suspect_valid;
} ntp_peer_t;

typedef struct {
    net_l4_endpoint endpoint;
    l3_id_t l3_id;
    uint64_t rate_until_mono_us;
    uint64_t last_seen_mono_us;
    uint8_t rate_backoff;
    bool denied;
    bool used;
} ntp_rule_t;

typedef struct {
    socket_handle_t sock;
    uint64_t t1_us;
    uint64_t orig_ntp64_be;
    bool active;
} ntp_query_t;

//https://www.rfc-editor.org/info/rfc5905/
//TODO full RFC 5905 clock filter and multisource select
static ntp_peer_t g_peers[NTP_MAX_CANDIDATES];
static ntp_rule_t g_rules[NTP_MAX_RULES];
static uint32_t g_failed_l2_mask;
static uint8_t g_selected_l2_ifindex;

static uint64_t unix_us_to_ntp64_be(uint64_t unix_us){
    uint64_t sec = unix_us / 1000000ULL;
    uint64_t frac = ((unix_us % 1000000ULL) << 32) / 1000000ULL;
    sec += NTP_UNIX_EPOCH_DELTA;
    uint64_t ntp = (sec << 32) | (frac & 0xffffffffULL);
    return be64(ntp);
}

static uint64_t ntp64_be_to_unix_us(uint64_t ntp_be){
    uint64_t ntp = be64(ntp_be);
    uint64_t sec = ntp >> 32;
    uint64_t frac = ntp & 0xffffffffULL;
    if (sec < NTP_UNIX_EPOCH_DELTA) sec += 1ULL << 32;
    sec -= NTP_UNIX_EPOCH_DELTA;
    return sec * 1000000ULL + ((frac * 1000000ULL) >> 32);
}

static uint64_t ntp_root_delay_be_to_us(uint32_t v_be, uint8_t vn){
    uint32_t v = be32(v_be);
    if (vn == 3 && (v & 0x80000000u)) return 0;
    return ((uint64_t)v * 1000000ULL) / 65536ULL;
}

static inline uint64_t ntp_local_time_us(void){
    uint64_t u = timer_unix_time_us();
    if (u) return u;
    return timer_wall_time_us();
}

static ntp_result_t ntp_send_query(socket_handle_t sock, const ntp_server_t* server, uint64_t* t1_us_out, uint64_t* tx_ntp64_be_out){
    ntp_packet_t p;
    memset(&p, 0, sizeof(p));
    p.li_vn_mode = ((uint8_t)NTP_VN << 3) | (uint8_t)NTP_MODE_CLIENT;
    p.poll = 6;
    p.precision = -20;

    uint64_t t1_us = ntp_local_time_us();
    uint64_t tx_be = unix_us_to_ntp64_be(t1_us);
    p.txTs = tx_be;
    int64_t sent = send_to_socket(sock, &server->endpoint, &p, sizeof(p));
    if (sent != (int64_t)sizeof(p)) return NTP_ERR_SEND;
    *t1_us_out = t1_us;
    *tx_ntp64_be_out = tx_be;
    return NTP_OK;
}

static bool ntp_valid_server_response(const ntp_packet_t* r, uint64_t expected_orig_ntp64_be, bool* kod, uint32_t* kod_refid){
    *kod = 0;
    *kod_refid = 0;
    if (!r) return 0;
    if ((r->li_vn_mode & 0x7) != NTP_MODE_SERVER) return 0;
    uint8_t vn = (r->li_vn_mode >> 3) & 0x7;
    if (vn < 3 || vn > 4) return 0;
    if (r->origTs != expected_orig_ntp64_be) return false;
    if (r->stratum== 0) {
        *kod = true;
        *kod_refid = be32(r->refId);
        return 0;
    }

    uint8_t li = (uint8_t)(r->li_vn_mode >> 6);
    if (li == 3) return 0;

    if (r->stratum >= 16) return 0;
    if (r->recvTs == 0 || r->txTs == 0) return 0;
    uint64_t rx = ntp64_be_to_unix_us(r->recvTs);
    uint64_t tx = ntp64_be_to_unix_us(r->txTs);
    if (tx < rx) return 0;
    uint64_t ref = r->refTs ? ntp64_be_to_unix_us(r->refTs) : 0;
    if (ref > tx) return 0;

    uint64_t root_delay_us = ntp_root_delay_be_to_us(r->rootDelay, vn);
    uint64_t root_disp_us = ((uint64_t)be32(r->rootDispersion) * 1000000ULL) / 65536ULL;
    uint64_t root_distance_us = root_delay_us / 2ULL + root_disp_us;
    if (root_distance_us >= NTP_MAX_DISP_US) return 0;
    return 1;
}

static bool ntp_server_l3_valid(const ntp_server_t* server) {
    if (server->endpoint.ver == IP_VER4) {
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(server->l3_id);
        return ipv4_l3_is_ready(v4) && v4->epoch == server->l3_epoch;
    }

    if (server->endpoint.ver == IP_VER6) {
        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(server->l3_id);
        return ipv6_l3_is_ready(v6) && v6->epoch == server->l3_epoch;
    }

    return false;
}

static void ntp_server_add(ntp_server_t* out, uint32_t* count, const void* ip, ip_version_t ver, l3_id_t l3_id, uint32_t l3_epoch) {
    if (*count >= NTP_MAX_CANDIDATES) return;
    ntp_server_t candidate = {0};
    make_ep(ip, NTP_PORT, ver, &candidate.endpoint);
    candidate.l3_id = l3_id;
    candidate.l3_epoch = l3_epoch;
    for (uint32_t i = 0; i < *count; i++) {
        if (out[i].l3_id != candidate.l3_id || out[i].l3_epoch != candidate.l3_epoch) continue;
        if (net_ep_equal(&out[i].endpoint, &candidate.endpoint)) return;
    }
    out[*count] = candidate;
    (*count)++;
}

static uint32_t discover_servers(ntp_server_t out[NTP_MAX_CANDIDATES], uint32_t skip_l2_mask) {
    ntp_server_t fallback[NTP_MAX_CANDIDATES] = {0};
    uint32_t fallback_count = 0;
    uint8_t fallback_l2_ifindex = 0;

    uint8_t l2n = l2_interface_count();
    for (uint8_t i = 0; i < l2n; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2 || !l2->is_up) continue;
        if (l2->ifindex && l2->ifindex <= MAX_L2_INTERFACES && (skip_l2_mask & (1u << (l2->ifindex - 1)))) continue;

        ntp_server_t v4[NTP_MAX_CANDIDATES] = {0};
        ntp_server_t v6[NTP_MAX_CANDIDATES] = {0};
        uint32_t v4n = 0;
        uint32_t v6n = 0;
        for (int s = 0; s < (int)N_ARR(l2->l3_v4) && v4n < NTP_MAX_CANDIDATES; s++) {
            l3_ipv4_interface_t* ip = l2->l3_v4[s];
            if (!ipv4_l3_is_ready(ip) || ip->is_localhost) continue;
            for (uint32_t n = 0; n < N_ARR(ip->runtime_opts_v4.ntp) && v4n < NTP_MAX_CANDIDATES; n++) { 
                uint32_t server = ip->runtime_opts_v4.ntp[n];
                if (server) ntp_server_add(v4, &v4n, &server, IP_VER4, ip->l3_id, ip->epoch);
            }
        }

        for (int s = 0; s < (int)N_ARR(l2->l3_v6) && v6n < NTP_MAX_CANDIDATES; s++) { 
            l3_ipv6_interface_t* ip = l2->l3_v6[s];
            if (!ipv6_l3_is_ready(ip) || ip->is_localhost || ipv6_is_linklocal(ip->ip)) continue;
            for (uint32_t n = 0; n < N_ARR(ip->runtime_opts_v6.ntp) && v6n < NTP_MAX_CANDIDATES; n++) {
                const uint8_t* server = ip->runtime_opts_v6.ntp[n];
                if (ipv6_is_unspecified(server) || ipv6_is_multicast(server)) continue;
                l3_id_t src_l3 = ip->l3_id;
                uint32_t src_epoch = ip->epoch;
                if (ipv6_is_linklocal(server)) {
                    src_l3 = 0;
                    src_epoch = 0;
                    for (int k = 0; k < (int)N_ARR(l2->l3_v6); k++) {
                        l3_ipv6_interface_t* candidate = l2->l3_v6[k];
                        if (!ipv6_l3_is_ready(candidate) || !ipv6_is_linklocal(candidate->ip)) continue;
                        src_l3 = candidate->l3_id;
                        src_epoch = candidate->epoch;
                        break;
                    }
                }
                if (src_l3) ntp_server_add(v6, &v6n, server, IP_VER6, src_l3, src_epoch);
            }
        }

        if (v4n && v6n) {
            uint32_t n = 0;
            uint32_t v4i = 0;
            uint32_t v6i = 0;
            while (n < NTP_MAX_CANDIDATES && (v4i < v4n || v6i < v6n)) {
                if (v4i < v4n) {
                    out[n++] = v4[v4i];
                    v4i++;
                }
                if (n < NTP_MAX_CANDIDATES && v6i < v6n) {
                    out[n++] = v6[v6i];
                    v6i++;
                }
            }
            g_selected_l2_ifindex = l2->ifindex;
            return n;
        }

        if (!fallback_count) {
            fallback_l2_ifindex = l2->ifindex;
            if (v4n) for (uint32_t n = 0; n < v4n && fallback_count < NTP_MAX_CANDIDATES; n++) fallback[fallback_count++] = v4[n];
            else if (v6n) for (uint32_t n = 0; n < v6n && fallback_count < NTP_MAX_CANDIDATES; n++) fallback[fallback_count++] = v6[n];
        }
    }

    for (uint32_t i = 0; i < fallback_count; i++) out[i] = fallback[i];
    if (fallback_count) g_selected_l2_ifindex = fallback_l2_ifindex;

    return fallback_count;
}

static ntp_rule_t* ntp_rule_for(const ntp_server_t* server, uint64_t now_us) {
    ntp_rule_t* empty = 0;
    ntp_rule_t* evict = 0;
    ntp_rule_t* old_denied = 0;
    for (uint32_t i = 0; i < NTP_MAX_RULES; i++) {
        ntp_rule_t* r = &g_rules[i];
        if (r->used && r->l3_id == server->l3_id && net_ep_equal(&r->endpoint, &server->endpoint)) {
            r->last_seen_mono_us = now_us;
            return r;
        }
        if (!r->used && !empty) empty = r;
        if (!r->used) continue;
        if (!r->denied && now_us < r->rate_until_mono_us) continue;
        int selected = 0;
        for (uint32_t j = 0; j < NTP_MAX_CANDIDATES; j++) {
            if (g_peers[j].server.l3_id == r->l3_id && net_ep_equal(&r->endpoint, &g_peers[j].server.endpoint)) {
                selected = 1;
                break;
            }
        }
        if (selected) continue;
        if (r->denied) {
            if (!old_denied || r->last_seen_mono_us < old_denied->last_seen_mono_us) old_denied = r;
        } else if (!evict || r->last_seen_mono_us < evict->last_seen_mono_us) evict = r;
    }
    ntp_rule_t* r = empty ? empty : (evict ? evict : old_denied);
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));
    r->used = true;
    r->endpoint = server->endpoint;
    r->l3_id = server->l3_id;
    r->last_seen_mono_us = now_us;
    return r;
}

static bool ntp_clock_outlier(uint64_t server_us) {
    if (!timer_is_synchronised()) return false;
    uint64_t wall_us = timer_unix_time_us();
    if (!wall_us) return false;
    uint64_t diff = server_us > wall_us ? server_us - wall_us : wall_us - server_us;
    return diff > NTP_CLOCK_GUARD_US;
}

static void ntp_peer_reset(ntp_peer_t* peer) {
    memset(peer->filt, 0, sizeof(peer->filt));
    peer->count = 0;
    peer->best_delay_us = 0;
    peer->jitter_us = 0;
    peer->root_distance_us = 0;
    peer->best_offset_us = 0;
    peer->best_sample_mono_us = 0;
    peer->best_server_time_us = 0;
    peer->last_sample_mono_us = 0;
    peer->last_server_time_us = 0;
    peer->last_filt_offset_us = 0;
    peer->suspect_valid = false;
    peer->suspect_votes = 0;
}

static void discipline_apply(ntp_peer_t* best_peer, uint64_t server_unix_us_at_t4){
    if (!best_peer) return;
    if (!best_peer->best_sample_mono_us || best_peer->best_sample_mono_us <= best_peer->last_sample_mono_us) return;

    int64_t off = best_peer->best_offset_us;
    uint64_t sample_mono_us = best_peer->best_sample_mono_us;
    uint64_t sample_server_us = best_peer->best_server_time_us;

    if (!timer_is_synchronised()) {
        timer_sync_set_unix_us(server_unix_us_at_t4);
        timer_sync_set_freq_ppm(0);
        for (uint32_t i = 0; i < NTP_MAX_CANDIDATES; i++) ntp_peer_reset(&g_peers[i]);
        return;
    }

    if (abs_i64(off) > NTP_STEP_US) {
        timer_sync_set_unix_us(server_unix_us_at_t4);
        for (uint32_t i = 0; i < NTP_MAX_CANDIDATES; i++) ntp_peer_reset(&g_peers[i]);
        return;
    }

    timer_sync_slew_us(off);

    if (best_peer->last_sample_mono_us != 0 && sample_server_us > best_peer->last_server_time_us) {
        uint64_t dt = sample_mono_us - best_peer->last_sample_mono_us;
        if (dt >= NTP_FREQ_MIN_INTERVAL_US) {
            int64_t thr = (int64_t)best_peer->jitter_us * 4LL + 2000LL;
            if (thr < 2000) thr = 2000;

            if (abs_i64(off) <= thr && abs_i64(best_peer->last_filt_offset_us) <= thr) {
                int64_t server_dt = (int64_t)(sample_server_us - best_peer->last_server_time_us);
                int64_t ppm_est = ((server_dt - (int64_t)dt) * 1000000LL) / (int64_t)dt;
                if (ppm_est > NTP_FREQ_MAX_PPM) ppm_est = NTP_FREQ_MAX_PPM;
                if (ppm_est < -NTP_FREQ_MAX_PPM) ppm_est = -NTP_FREQ_MAX_PPM;

                int32_t current = timer_sync_get_freq_ppm();
                int32_t freq_ppm = (int32_t)(((int64_t)current * 7LL + ppm_est) / 8LL);
                timer_sync_set_freq_ppm(freq_ppm);
            }
        }
    }

    best_peer->last_sample_mono_us = sample_mono_us;
    best_peer->last_server_time_us = sample_server_us;
    best_peer->last_filt_offset_us = off;
}

ntp_result_t ntp_poll_once(uint32_t timeout_ms){
    ntp_server_t servers[NTP_MAX_CANDIDATES] = {0};
    uint32_t server_count = discover_servers(servers, g_failed_l2_mask);

    if (!server_count && g_failed_l2_mask) {
        g_failed_l2_mask = 0;
        server_count = discover_servers(servers, 0);
    }
    if (!server_count) {
        g_selected_l2_ifindex = 0;
        return NTP_ERR_NO_SERVER;
    }

    ntp_peer_t next[NTP_MAX_CANDIDATES];
    memset(next, 0, sizeof(next));
    for (uint32_t i = 0; i < server_count; i++) { 
        for (uint32_t j = 0; j < NTP_MAX_CANDIDATES; j++) {
            if (!g_peers[j].server.l3_id || g_peers[j].server.l3_id != servers[i].l3_id || g_peers[j].server.l3_epoch != servers[i].l3_epoch) continue;
            if (!net_ep_equal(&g_peers[j].server.endpoint, &servers[i].endpoint)) continue;
            next[i] = g_peers[j];
            break;
        }
        next[i].server = servers[i];
    }
    memcpy(g_peers, next, sizeof(g_peers));

    ntp_result_t best_err = NTP_ERR_KOD;

    uint32_t tried = 0;
    uint32_t normal_samples = 0;
    bool new_suspect = false;
    bool valid_response = false;
    bool peer_suppressed = false;


    while (true) {
        ntp_query_t query[NTP_MAX_CANDIDATES];
        memset(query, 0, sizeof(query));
        uint32_t active = 0;
        uint64_t mono_now_us = timer_now_usec();

        for (uint32_t i = 0; i < server_count && active < 2; i++) {
            if (tried & (1u << i)) continue;
            ntp_peer_t* peer = &g_peers[i];
            ntp_rule_t* rule = ntp_rule_for(&peer->server, mono_now_us);
            if (!rule || rule->denied || mono_now_us < rule->rate_until_mono_us) {
                peer_suppressed = true;
                continue;
            }
            tried |= 1u << i;
            if (!ntp_server_l3_valid(&peer->server)) {
                best_err = NTP_ERR_NO_SERVER;
                continue;
            }

            socket_handle_t sock = create_socket(PROTO_UDP, &(SocketOptions){.flags = SOCK_OPT_NONBLOCK});
            if (!sock) {
                best_err = NTP_ERR_SOCKET;
                continue;
            }
            SockBindSpec spec = {.kind = BIND_L3, .ver = peer->server.endpoint.ver, .l3_id = peer->server.l3_id};
            if (bind_socket(sock, &spec, 0) != SOCK_OK) {
                close_socket(sock);
                best_err = NTP_ERR_SOCKET;
                continue;
            }
            if (!ntp_server_l3_valid(&peer->server)) {
                close_socket(sock);
                best_err = NTP_ERR_NO_SERVER;
                continue;
            }
            if (ntp_send_query(sock, &peer->server, &query[i].t1_us, &query[i].orig_ntp64_be) != NTP_OK) {
                close_socket(sock);
                best_err = NTP_ERR_SEND;
                continue;
            }
            query[i].sock = sock;
            query[i].active = true;
            active++;
        }

        if (!active) break;
        best_err = NTP_ERR_TIMEOUT;

        uint64_t wait_start_us = timer_now_usec();
        while (active && timer_now_usec() - wait_start_us < (uint64_t)timeout_ms * 1000ULL) {
            bool progress = false;
            for (uint32_t i = 0; i < server_count; i++) {
                if (!query[i].active) continue;
                uint8_t buf[96];
                net_l4_endpoint src;
                int64_t n = receive_from_socket(query[i].sock, buf, sizeof(buf), &src);

                if (n >= 0) progress = true;
                if (n < (int64_t)sizeof(ntp_packet_t)) continue;
                uint64_t t4_us = ntp_local_time_us();
                uint64_t mono_sample_us = timer_now_usec();
                if (!net_ep_equal(&src,&g_peers[i].server.endpoint)) continue;
                if (!ntp_server_l3_valid(&g_peers[i].server)) {
                    close_socket(query[i].sock);
                    query[i].sock = 0;
                    query[i].active = false;
                    active--;
                    best_err = NTP_ERR_NO_SERVER;
                    continue;
                }

                const ntp_packet_t* r = (const ntp_packet_t*)buf;
                bool kod = false;
                uint32_t kod_refid = 0;
                if (!ntp_valid_server_response(r, query[i].orig_ntp64_be, &kod, &kod_refid)) {
                    if (kod) {
                        ntp_rule_t* rule = ntp_rule_for(&g_peers[i].server, mono_sample_us);
                        if (rule) {
                            if (kod_refid == NTP_REFID_DENY || kod_refid == NTP_REFID_RSTR) {
                                rule->denied = true;
                            } else if (kod_refid == NTP_REFID_RATE) {
                                peer_suppressed = true;
                                uint64_t rate_sec = (uint64_t)NTP_RATE_MIN_SEC << rule->rate_backoff;
                                if (rule->rate_backoff < NTP_RATE_MAX_SHIFT) rule->rate_backoff++;
                                rule->rate_until_mono_us = mono_sample_us + rate_sec * 1000000ULL;
                            }
                        }
                        best_err = NTP_ERR_KOD;
                        close_socket(query[i].sock);
                        query[i].sock = 0;
                        query[i].active = false;
                        active--;
                    }
                    continue;
                }

                uint64_t t2_us = ntp64_be_to_unix_us(r->recvTs);
                uint64_t t3_us = ntp64_be_to_unix_us(r->txTs);
                int64_t t1 = (int64_t)query[i].t1_us;
                int64_t t2 = (int64_t)t2_us;
                int64_t t3 = (int64_t)t3_us;
                int64_t t4 = (int64_t)t4_us;
                int64_t rtt = (t4 - t1) - (t3 - t2);
                if (rtt < -2000) continue;
                if (rtt < 0) rtt = 0;
                int64_t off = ((t2 - t1) + (t3 - t4)) / 2;
                int64_t server_time = t4 + off;

                const uint64_t year2000 = 946684800ULL*1000000ULL;
                if (server_time < (int64_t)year2000) continue;

                uint64_t server_unix_us = (uint64_t)server_time;
                valid_response = true;
                ntp_peer_t* p = &g_peers[i];
                uint8_t vn = (r->li_vn_mode >> 3) & 0x7;
                uint64_t root_delay_us = ntp_root_delay_be_to_us(r->rootDelay, vn);
                uint64_t root_disp_us = ((uint64_t) be32(r->rootDispersion) * 1000000ULL) / 65536ULL;
                uint64_t distance_us = root_delay_us / 2ULL + root_disp_us + (uint64_t)rtt / 2ULL + 1000ULL;
                ntp_rule_t* rule = ntp_rule_for(&p->server, mono_sample_us);
                if (rule) {
                    rule->rate_backoff = 0;
                    rule->rate_until_mono_us = 0;
                }
                if (ntp_clock_outlier(server_unix_us)) {
                    if ((uint64_t)rtt <= NTP_RECOVERY_MAX_RTT_US && distance_us <= NTP_RECOVERY_MAX_DISTANCE_US) { 
                        uint8_t votes = 1;
                        if (p->suspect_valid && mono_sample_us > p->suspect_mono_us + 1000000ULL && mono_sample_us - p->suspect_mono_us <= NTP_RECOVERY_FRESH_US) { 
                            uint64_t predicted = p->suspect_server_us + mono_sample_us - p->suspect_mono_us;
                            uint64_t diff = predicted > server_unix_us ? predicted - server_unix_us : server_unix_us - predicted;
                            if (diff <= NTP_RECOVERY_AGREEMENT_US) votes = 2;
                        }
                        p->suspect_mono_us = mono_sample_us;
                        p->suspect_server_us = server_unix_us;
                        p->suspect_distance_us = distance_us;
                        p->suspect_votes = votes;
                        p->suspect_valid = true;
                        new_suspect = true;
                    } else {
                        p->suspect_valid = 0;
                        p->suspect_votes = 0;
                    }
                    best_err = NTP_ERR_FORMAT;
                } else { 
                    p->suspect_valid = 0;
                    p->suspect_votes = 0;
                    ntp_sample_t s;
                    s.offset_us = off;
                    s.delay_us = (uint64_t)rtt;
                    s.dispersion_us = 1000;
                    s.mono_time_us = mono_sample_us;
                    s.server_time_us = server_unix_us;
                    s.root_delay_us = root_delay_us;
                    s.root_dispersion_us = root_disp_us;

                    for (int j = (int)NTP_FILTER_N - 1; j > 0; j--) p->filt[j] = p->filt[j - 1];
                    p->filt[0] = s;
                    if (p->count < NTP_FILTER_N) p->count++;

                    uint8_t best = 0;
                    uint64_t best_delay = UINT64_MAX;
                    uint64_t best_disp = UINT64_MAX;

                    for (uint8_t j = 0; j < p->count; j++) {
                        uint64_t age_us = mono_sample_us -p->filt[j].mono_time_us;
                        uint64_t grow = (age_us * (uint64_t)NTP_PHI_PPM) / 1000000ULL;
                        uint64_t disp = p->filt[j].dispersion_us + grow;
                        uint64_t delay = p->filt[j].delay_us;

                        if (delay < best_delay || (delay == best_delay && disp < best_disp)) {
                            best = j;
                            best_delay = delay;
                            best_disp = disp;
                        }
                    }

                    int64_t best_off = p->filt[best].offset_us;
                    uint64_t sumsq = 0;
                    for (uint8_t j = 0; j < p->count; j++) {
                        int64_t d = p->filt[j].offset_us - best_off;
                        uint64_t a = (uint64_t)abs_i64(d);
                        if (a > UINT32_MAX) a = UINT32_MAX;
                        if (UINT64_MAX - sumsq < a * a) sumsq = UINT64_MAX;
                        else sumsq += a * a;
                    }

                    p->best_offset_us = best_off;
                    p->best_sample_mono_us = p->filt[best].mono_time_us;
                    p->best_server_time_us = p->filt[best].server_time_us;
                    p->best_delay_us = best_delay;
                    uint64_t jitter_n = p->count > 1 ? (uint64_t)p->count - 1ULL : 1ULL;
                    p->jitter_us = sqrt_u64(sumsq / jitter_n);

                    uint64_t root_dist = p->filt[best].root_dispersion_us;
                    root_dist += p->filt[best].root_delay_us / 2ULL;
                    root_dist += best_delay / 2ULL;
                    root_dist += best_disp;
                    root_dist += p->jitter_us;
                    p->root_distance_us = root_dist;
                    normal_samples |= 1u << i;
                    best_err = NTP_OK;
                }
                close_socket(query[i].sock);
                query[i].sock = 0;
                query[i].active = false;
                active--;
                progress = true;
            }

            if (!active) break;
            if (!progress) {
                uint64_t elapsed_us = timer_now_usec() - wait_start_us;
                uint64_t timeout_us = (uint64_t)timeout_ms * 1000ULL;
                if (elapsed_us >= timeout_us) break;
                uint32_t step = (uint32_t)((timeout_us - elapsed_us + 999ULL) / 1000ULL);
                if (step > 50) step = 50;
                if (step) msleep(step);
            }
        }

        for (uint32_t i = 0; i < server_count; i++) if (query[i].sock) close_socket(query[i].sock);
        if (normal_samples && !new_suspect) break;
    }

    uint64_t recovery_time_us = 0;
    bool recovered = 0;
    uint64_t now_us = timer_now_usec();
    if (new_suspect && !normal_samples) {
        for (uint32_t i = 0; i < server_count; i++) {
            ntp_peer_t* a = &g_peers[i];
            if (!a->suspect_valid || a->suspect_votes < 2 || now_us < a->suspect_mono_us || now_us - a->suspect_mono_us > NTP_RECOVERY_FRESH_US) continue;
            ntp_rule_t* ar = ntp_rule_for(&a->server, now_us);
            if (!ar || ar->denied || now_us < ar->rate_until_mono_us) continue;
            uint64_t a_now = a->suspect_server_us + now_us - a->suspect_mono_us;
            if (!ntp_clock_outlier(a_now)) continue;
            for (uint32_t j = i+1; j < server_count; j++) {
                ntp_peer_t* b = &g_peers[j];
                if (!b->suspect_valid || b->suspect_votes < 2 || now_us < b->suspect_mono_us || now_us - b->suspect_mono_us > NTP_RECOVERY_FRESH_US) continue;
                ntp_rule_t* br = ntp_rule_for(&b->server, now_us);
                if (!br || br->denied || now_us < br->rate_until_mono_us) continue;
                if (a->server.endpoint.ver != b->server.endpoint.ver || net_ep_equal(&a->server.endpoint, &b->server.endpoint)) continue;
                uint64_t b_now = b->suspect_server_us + now_us - b->suspect_mono_us;
                if (!ntp_clock_outlier(b_now)) continue;
                uint64_t diff = a_now > b_now ? a_now - b_now : b_now - a_now;
                if (diff > NTP_RECOVERY_AGREEMENT_US) continue;
                recovery_time_us = a->suspect_distance_us <= b->suspect_distance_us ? a_now : b_now;
                recovered = 1;
                break;
            }
            if (recovered)break;
        }
    }

    if (recovered) {
        timer_sync_set_freq_ppm(0);
        timer_sync_set_unix_us(recovery_time_us);
        for (uint32_t i = 0; i < NTP_MAX_CANDIDATES; i++) ntp_peer_reset(&g_peers[i]);
        return NTP_OK;
    }

    ntp_peer_t* best = NULL;
    for (uint32_t i = 0; i < server_count; i++) {
        if (!(normal_samples & (1u << i)) || !g_peers[i].count) continue;
        if (!best) best = &g_peers[i];
        else {
            uint64_t da = g_peers[i].root_distance_us;
            uint64_t db = best->root_distance_us;
            if (da < db || (da == db && g_peers[i].best_delay_us < best->best_delay_us)) best = &g_peers[i];
        }
    }

    if (!best) {
        if (!peer_suppressed && !valid_response && g_selected_l2_ifindex && g_selected_l2_ifindex <= MAX_L2_INTERFACES) g_failed_l2_mask |= 1u << (g_selected_l2_ifindex - 1);
        return best_err;
    }

    uint64_t t4_us = ntp_local_time_us();
    uint64_t server_unix_us = (uint64_t)((int64_t)t4_us + best->best_offset_us);

    discipline_apply(best, server_unix_us);

    return NTP_OK;
}

uint8_t ntp_max_filter_count(void) {
    uint64_t now = timer_now_usec();
    uint8_t max_count = 0;
    for (uint32_t i = 0; i < NTP_MAX_CANDIDATES; i++) {
        if (!g_peers[i].server.l3_id) continue;
        ntp_rule_t* rule = ntp_rule_for(&g_peers[i].server, now);
        if (!rule || rule->denied || now < rule->rate_until_mono_us) continue;
        if (g_peers[i].count > max_count) max_count = g_peers[i].count;
    }
    return max_count;
}
