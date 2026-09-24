#include "ntp.h"
#include "exceptions/timer.h"
#include "std/memory.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "process/scheduler.h"
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
    uint64_t rate_until_mono_us;
    uint8_t rate_backoff;
    bool denied;
} ntp_peer_t;

typedef struct {
    socket_handle_t sock;
    uint64_t t1_us;
    uint64_t orig_ntp64_be;
    bool active;
    bool sampled;
} ntp_query_t;

//https://www.rfc-editor.org/info/rfc5905/
//TODO full RFC 5905 clock filter and multisource select
static ntp_peer_t g_peers[2];
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
    if (*count >= 2) return;
    ntp_server_t candidate = {0};
    make_ep(ip, NTP_PORT, ver, &candidate.endpoint);
    candidate.l3_id = l3_id;
    candidate.l3_epoch = l3_epoch;
    for (uint32_t i = 0; i < *count; i++) {
        if (out[i].l3_id != candidate.l3_id || out[i].l3_epoch != candidate.l3_epoch) continue;
        if (out[i].endpoint.ver != candidate.endpoint.ver || out[i].endpoint.port != candidate.endpoint.port) continue;
        uint32_t ip_len = candidate.endpoint.ver == IP_VER6 ? 16 : 4;
        if (memcmp(out[i].endpoint.ip, candidate.endpoint.ip, ip_len) == 0) return;
    }
    out[*count] = candidate;
    (*count)++;
}

static uint32_t discover_servers(ntp_server_t out[2], uint32_t skip_l2_mask) {
    ntp_server_t fallback[2] = {0};
    uint32_t fallback_count = 0;
    uint8_t fallback_l2_ifindex = 0;

    uint8_t l2n = l2_interface_count();
    for (uint8_t i = 0; i < l2n; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2 || !l2->is_up) continue;
        if (l2->ifindex && l2->ifindex <= MAX_L2_INTERFACES && (skip_l2_mask & (1u << (l2->ifindex - 1)))) continue;

        ntp_server_t v4[2] = {0};
        ntp_server_t v6[2] = {0};
        uint32_t v4n = 0;
        uint32_t v6n = 0;
        for (int s = 0; s < MAX_IPV4_PER_INTERFACE && v4n < 2; s++) {
            l3_ipv4_interface_t* ip = l2->l3_v4[s];
            if (!ipv4_l3_is_ready(ip) || ip->is_localhost) continue;
            for (uint32_t n = 0; n < 2 && v4n < 2; n++) { 
                uint32_t server = ip->runtime_opts_v4.ntp[n];
                if (server) ntp_server_add(v4, &v4n, &server, IP_VER4, ip->l3_id, ip->epoch);
            }
        }

        for (int s = 0; s < MAX_IPV6_PER_INTERFACE && v6n < 2; s++) { 
            l3_ipv6_interface_t* ip = l2->l3_v6[s];
            if (!ipv6_l3_is_ready(ip) || ip->is_localhost || ipv6_is_linklocal(ip->ip)) continue;
            for (uint32_t n = 0; n < 2 && v6n < 2; n++) {
                const uint8_t* server = ip->runtime_opts_v6.ntp[n];
                if (ipv6_is_unspecified(server) || ipv6_is_multicast(server)) continue;
                l3_id_t src_l3 = ip->l3_id;
                uint32_t src_epoch = ip->epoch;
                if (ipv6_is_linklocal(server)) {
                    src_l3 = 0;
                    src_epoch = 0;
                    for (int k = 0; k < MAX_IPV6_PER_INTERFACE; k++) {
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
            out[0] = v4[0];
            out[1] = v6[0];
            g_selected_l2_ifindex = l2->ifindex;
            return 2;
        }

        if (!fallback_count) {
            fallback_l2_ifindex = l2->ifindex;
            if (v4n) for (uint32_t n = 0; n < v4n && fallback_count < 2; n++) fallback[fallback_count++] = v4[n];
            else if (v6n) for (uint32_t n = 0; n < v6n && fallback_count < 2; n++) fallback[fallback_count++] = v6[n];
        }
    }

    for (uint32_t i = 0; i < fallback_count; i++) out[i] = fallback[i];
    if (fallback_count) g_selected_l2_ifindex = fallback_l2_ifindex;

    return fallback_count;
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
        for (uint32_t i = 0; i < 2; i++) ntp_peer_reset(&g_peers[i]);
        return;
    }

    if (abs_i64(off) > NTP_STEP_US) {
        timer_sync_set_unix_us(server_unix_us_at_t4);
        for (uint32_t i = 0; i < 2; i++) ntp_peer_reset(&g_peers[i]);
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
    ntp_server_t servers[2] = {0};
    uint32_t server_count = discover_servers(servers, g_failed_l2_mask);

    if (!server_count && g_failed_l2_mask) {
        g_failed_l2_mask = 0;
        server_count = discover_servers(servers, 0);
    }
    if (!server_count) {
        g_selected_l2_ifindex = 0;
        return NTP_ERR_NO_SERVER;
    }

    ntp_peer_t next[2];
    memset(next, 0, sizeof(next));
    for (uint32_t i = 0; i < server_count; i++) { 
        bool found = false;
        for (uint32_t j = 0; j < 2; j++) {
            if (!g_peers[j].server.l3_id || g_peers[j].server.l3_id != servers[i].l3_id || g_peers[j].server.l3_epoch != servers[i].l3_epoch) continue;
            if (g_peers[j].server.endpoint.ver != servers[i].endpoint.ver || g_peers[j].server.endpoint.port != servers[i].endpoint.port) continue;
            uint32_t ip_len = servers[i].endpoint.ver == IP_VER6 ? 16 : 4;
            if (memcmp(g_peers[j].server.endpoint.ip, servers[i].endpoint.ip, ip_len) != 0) continue;
            next[i] = g_peers[j];
            found = true;
            break;
        }
        if (!found) next[i].server = servers[i];
    }
    memcpy(g_peers, next, sizeof(g_peers));

    ntp_query_t query[2];
    memset(query, 0, sizeof(query));
    ntp_result_t best_err = NTP_ERR_KOD;

    uint64_t mono_now_us = timer_now_usec();
    uint32_t active = 0;
    bool peer_suppressed = false;

    for (uint32_t i = 0; i < server_count; i++) {
        ntp_peer_t* peer = &g_peers[i];
        if (peer->denied) continue;
        if (peer->rate_until_mono_us && mono_now_us < peer->rate_until_mono_us) {
            peer_suppressed = true;
            continue;
        }
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

    if (!active) {
        if (!peer_suppressed && g_selected_l2_ifindex && g_selected_l2_ifindex <= MAX_L2_INTERFACES) g_failed_l2_mask |= 1u << (g_selected_l2_ifindex - 1);
        return best_err;
    }
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
            if (src.port != NTP_PORT || src.ver != g_peers[i].server.endpoint.ver) continue;
            uint32_t src_len = src.ver == IP_VER6 ? 16 : 4;
            if (memcmp(src.ip, g_peers[i].server.endpoint.ip, src_len) != 0) continue;
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
                    if (kod_refid == NTP_REFID_DENY || kod_refid == NTP_REFID_RSTR) {
                        g_peers[i].denied = true;
                    } else if (kod_refid == NTP_REFID_RATE) {
                        peer_suppressed = true;
                        uint64_t rate_sec = (uint64_t)NTP_RATE_MIN_SEC << g_peers[i].rate_backoff;
                        if (g_peers[i].rate_backoff < NTP_RATE_MAX_SHIFT) g_peers[i].rate_backoff++;
                        g_peers[i].rate_until_mono_us = timer_now_usec() + rate_sec * 1000000;
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
            if (timer_is_synchronised()) {
                uint64_t now_wall_us = timer_unix_time_us();
                uint64_t plus1d = 86400ULL * 1000000ULL;
                uint64_t wall_diff = server_unix_us > now_wall_us ? server_unix_us - now_wall_us : now_wall_us - server_unix_us;
                if (now_wall_us && wall_diff > plus1d) continue;
            }

            ntp_peer_t* p = &g_peers[i];

            ntp_sample_t s;
            s.offset_us = off;
            s.delay_us = (uint64_t)rtt;
            s.dispersion_us = 1000;
            s.mono_time_us = mono_sample_us;
            s.server_time_us = server_unix_us;
            uint8_t vn = (r->li_vn_mode >> 3) & 0x7;
            s.root_delay_us = ntp_root_delay_be_to_us(r->rootDelay, vn);
            s.root_dispersion_us = ((uint64_t)be32(r->rootDispersion) * 1000000ULL) / 65536ULL;

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
            p->rate_backoff = 0;
            p->rate_until_mono_us = 0;
            query[i].sampled = true;
            best_err = NTP_OK;
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

    ntp_peer_t* best = NULL;
    for (uint32_t i = 0; i < server_count; i++) {
        if (!query[i].sampled || !g_peers[i].count) continue;
        if (!best) best = &g_peers[i];
        else {
            uint64_t da = g_peers[i].root_distance_us;
            uint64_t db = best->root_distance_us;
            if (da < db || (da == db && g_peers[i].best_delay_us < best->best_delay_us)) best = &g_peers[i];
        }
    }

    if (!best) {
        if (!peer_suppressed && g_selected_l2_ifindex && g_selected_l2_ifindex <= MAX_L2_INTERFACES) g_failed_l2_mask |= 1u << (g_selected_l2_ifindex - 1);
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
    for (uint32_t i = 0; i < 2; i++) {
        if (!g_peers[i].server.l3_id || g_peers[i].denied) continue;
        if (g_peers[i].rate_until_mono_us && now < g_peers[i].rate_until_mono_us) continue;
        if (g_peers[i].count > max_count) max_count = g_peers[i].count;
    }
    return max_count;
}
