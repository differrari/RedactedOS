#include "ntp.h"
#include "exceptions/timer.h"
#include "std/memory.h"
#include "networking/internet_layer/ipv4.h"
#include "process/scheduler.h"
#include "console/kio.h"
#include "math/math.h"
#include "networking/transport_layer/csocket_udp.h"
#include "networking/transport_layer/trans_utils.h"

#include "syscalls/syscalls.h"

#define NTP_PORT 123
#define NTP_VN 4
#define NTP_MODE_CLIENT 3
#define NTP_MODE_SERVER 4
#define NTP_UNIX_EPOCH_DELTA 2208988800UL

#define NTP_STEP_US 128000LL
#define NTP_FREQ_MAX_PPM 500
#define NTP_PHI_PPM 15

#define NTP_REFID_DENY 0x44454E59u //DENY
#define NTP_REFID_RSTR 0x52535452u //RSTR

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
    uint32_t ip_host;
    uint64_t last_sample_mono_us;
    int64_t last_filt_offset_us;
    int32_t freq_ppm;
    ntp_sample_t filt[NTP_FILTER_N];
    uint8_t count;
    uint64_t best_delay_us;
    uint64_t best_disp_us;
    uint64_t jitter_us;
    uint64_t root_distance_us;
    int64_t best_offset_us;
    uint64_t root_delay_us;
    uint64_t root_disp_us;
} ntp_peer_t;

static ntp_peer_t g_peers[2];
static uint64_t g_last_ref_unix_us = 0;

static uint32_t g_best_ip_host = 0;
static uint64_t g_best_delay_us = 0;
static uint64_t g_best_disp_us = 0;
static uint64_t g_best_jitter_us = 0;
static uint64_t g_best_root_distance_us = 0;
static int64_t g_best_offset_us = 0;
static uint8_t g_best_count = 0;
static uint8_t g_max_count = 0;

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
    if (sec < NTP_UNIX_EPOCH_DELTA) return 0;
    sec -= NTP_UNIX_EPOCH_DELTA;
    return sec * 1000000ULL + ((frac * 1000000ULL) >> 32);
}

static uint64_t ntp_short_be_to_us_signed(uint32_t v_be){
    int32_t v = be32(v_be);
    if (v <= 0) return 0;
    uint64_t us = ((uint64_t)v * 1000000ULL) / 65536ULL;
    return us;
}

static inline uint64_t ntp_local_time_us(void){
    uint64_t u = timer_unix_time_us();
    if (u) return u;
    return timer_wall_time_us();
}

static ntp_result_t ntp_send_query(socket_handle_t sock, uint32_t server_ip_host, uint64_t* t1_us_out, uint64_t* tx_ntp64_be_out){
    ntp_packet_t p;
    memset(&p, 0, sizeof(p));
    p.li_vn_mode = (0u << 6) | ((uint8_t)NTP_VN << 3)|  (uint8_t)NTP_MODE_CLIENT;
    p.poll = 6;
    p.precision = -20;

    if (g_last_ref_unix_us) p.refTs = unix_us_to_ntp64_be(g_last_ref_unix_us);

    uint64_t t1_us = ntp_local_time_us();
    uint64_t tx_be = unix_us_to_ntp64_be(t1_us);
    p.txTs = tx_be;

    net_l4_endpoint dst;
    make_ep(server_ip_host, NTP_PORT, IP_VER4, &dst);
    int64_t sent = socket_sendto_udp_ex(sock, DST_ENDPOINT, &dst, 0, &p, sizeof(p));
    if (sent < 0) return NTP_ERR_SEND;
    *t1_us_out = t1_us;
    *tx_ntp64_be_out = tx_be;
    return NTP_OK;
}

static int ntp_valid_server_response(const ntp_packet_t* r, uint64_t expected_orig_ntp64_be, ntp_result_t* kod_out){
    if (!r) return 0;
    uint8_t li = (uint8_t)(r->li_vn_mode >> 6);
    if (li == 3) return 0;
    if ((r->li_vn_mode & 0x7) != NTP_MODE_SERVER) return 0;
    uint8_t vn = (r->li_vn_mode >> 3) & 0x7;
    if (vn < 3 || vn > 4) return 0;
    if (r->stratum== 0) {
        if (kod_out) {
            uint32_t rid = be32(r->refId);
            if (rid == NTP_REFID_DENY || rid == NTP_REFID_RSTR) *kod_out = NTP_ERR_KOD;
            else *kod_out = NTP_ERR_FORMAT;
        }
        return 0;
    }

    if (r->stratum >= 16) return 0;
    if (r->origTs != expected_orig_ntp64_be) return 0;
    if (r->recvTs == 0 || r->txTs == 0) return 0;
    uint64_t rx = ntp64_be_to_unix_us(r->recvTs);
    uint64_t tx = ntp64_be_to_unix_us(r->txTs);
    if (tx < rx) return 0;
    (void)kod_out;
    return 1;
}

static void discover_servers(uint32_t* s0, uint32_t* s1){
    *s0 = 0;
    *s1 = 0;

    uint8_t l2n = l2_interface_count();
    for (uint8_t i = 0; i < l2n && (*s0 == 0 || *s1 == 0); i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2) continue;
        for (int s = 0; s < MAX_IPV4_PER_INTERFACE && (*s0 == 0 || *s1 == 0); s++) {
            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
            if (!v4) continue;
            if (v4->mode == IPV4_CFG_DISABLED) continue;
            const net_runtime_opts_t* rt =&v4->runtime_opts_v4;
            if (!rt) continue;
            uint32_t c0 = rt->ntp[0];
            uint32_t c1 = rt->ntp[1];
            if (c0 && c0 != *s0 && c0 != *s1) {
                if (!*s0) *s0 = c0;
                else if (!*s1) *s1 = c0;
            }
            if (c1 && c1 != *s0 && c1 != *s1) {
                if (!*s0) *s0 = c1;
                else if (!*s1) *s1 = c1;
            }
        }
    }
}

static void discipline_apply(ntp_peer_t* best_peer, uint64_t server_unix_us_at_t4, uint64_t mono_now_us){
    if (!best_peer) return;

    int64_t off = best_peer->best_offset_us;
    g_last_ref_unix_us = server_unix_us_at_t4;

    if (!timer_is_synchronised()) {
        timer_sync_set_unix_us(server_unix_us_at_t4);
        timer_sync_set_freq_ppm(0);
        best_peer->last_sample_mono_us = mono_now_us;
        best_peer->last_filt_offset_us = off;
        best_peer->freq_ppm = 0;
        return;
    }

    if (abs_i64(off) > NTP_STEP_US) {
        timer_sync_set_unix_us(server_unix_us_at_t4);
        timer_sync_set_freq_ppm(0);

        best_peer->count = 0;
        best_peer->best_delay_us = 0;
        best_peer->best_disp_us = 0;
        best_peer->jitter_us = 0;
        best_peer->root_distance_us = 0;
        best_peer->best_offset_us = 0;
        best_peer->last_sample_mono_us = mono_now_us;
        best_peer->last_filt_offset_us = 0;
        best_peer->freq_ppm = 0;

        return;
    }

    timer_sync_slew_us(off);

    if (best_peer->last_sample_mono_us != 0) {
        uint64_t dt = mono_now_us - best_peer->last_sample_mono_us;
        if (dt >= 1000000ULL) {
            int64_t thr = (int64_t)best_peer->jitter_us * 4LL + 2000LL;
            if (thr < 2000) thr = 2000;

            if (abs_i64(off) <= thr && abs_i64(best_peer->last_filt_offset_us) <= thr) {
                int64_t d_off = off - best_peer->last_filt_offset_us;
                int64_t ppm_est = (d_off * 1000000LL) / (int64_t)dt;
                if (ppm_est > NTP_FREQ_MAX_PPM) ppm_est = NTP_FREQ_MAX_PPM;
                if (ppm_est < -NTP_FREQ_MAX_PPM) ppm_est = -NTP_FREQ_MAX_PPM;

                int32_t est = (int32_t)ppm_est;
                best_peer->freq_ppm = (int32_t)((best_peer->freq_ppm * 7 + est) / 8);
                if (best_peer->freq_ppm > NTP_FREQ_MAX_PPM) best_peer->freq_ppm = NTP_FREQ_MAX_PPM;
                if (best_peer->freq_ppm < -NTP_FREQ_MAX_PPM) best_peer->freq_ppm = -NTP_FREQ_MAX_PPM;
                timer_sync_set_freq_ppm(best_peer->freq_ppm);
            }
        }
    }

    best_peer->last_sample_mono_us = mono_now_us;
    best_peer->last_filt_offset_us = off;
}

ntp_result_t ntp_poll_once(uint32_t timeout_ms){
    uint32_t s0 = 0;
    uint32_t s1 = 0;

    discover_servers(&s0, &s1);
    if (s0 == 0 && s1 == 0) return NTP_ERR_NO_SERVER;

    socket_handle_t sock = udp_socket_create(0, (uint32_t)get_current_proc_pid(), NULL);
    if (sock == 0) return NTP_ERR_SOCKET;

    uint64_t t1_0 = 0, t1_1 = 0;
    uint64_t o0 = 0, o1 = 0;

    if (s0) {
        if (ntp_send_query(sock, s0, &t1_0, &o0) != NTP_OK) t1_0 = 0;
    }
    if (s1 && s1 != s0) {
        if (ntp_send_query(sock, s1, &t1_1, &o1) != NTP_OK) t1_1 = 0;
    }

    uint32_t waited = 0;
    ntp_result_t best_err = NTP_ERR_TIMEOUT;

    uint64_t mono_now_us = timer_now_usec();

    while (waited < timeout_ms) {
        uint8_t buf[96];
        net_l4_endpoint src;
        int64_t n = socket_recvfrom_udp_ex(sock, buf, sizeof(buf), &src);

        if (n >= (int64_t)sizeof(ntp_packet_t) && src.ver == IP_VER4 && src.port == NTP_PORT) {
            uint32_t rip = 0;
            memcpy(&rip, src.ip, 4);
            if (rip == s0 || rip == s1) {
                uint64_t t4_us = ntp_local_time_us();
                uint64_t mono_sample_us = timer_now_usec();
                const ntp_packet_t* r = (const ntp_packet_t*)buf;
                uint64_t t1_us = (rip == s0) ? t1_0 : t1_1;
                uint64_t orig_be = (rip == s0) ? o0 : o1;
                if (t1_us == 0) continue;

                ntp_result_t kod = NTP_OK;
                if (!ntp_valid_server_response(r, orig_be, &kod)) {
                    if (kod == NTP_ERR_KOD) {
                        uint32_t refid = be32(r->refId);
                        if (refid == NTP_REFID_DENY || refid == NTP_REFID_RSTR) best_err = NTP_ERR_KOD;
                    }
                    continue;
                }

                uint64_t T2 = ntp64_be_to_unix_us(r->recvTs);
                uint64_t T3 = ntp64_be_to_unix_us(r->txTs);
                if (T2 == 0 || T3 == 0) continue;

                int64_t rtt = (int64_t)(t4_us - t1_us) - (int64_t)(T3 - T2);
                if (rtt < 0) rtt = 0;
                int64_t off = ((int64_t)(T2 - t1_us) + (int64_t)(T3 - t4_us)) / 2;
                uint64_t server_unix_us = (uint64_t)((int64_t)t4_us + off);

                const uint64_t year2000 = 946684800ULL*1000000ULL;
                bool ok_range = false;
                if (timer_is_synchronised()) {
                    uint64_t now_wall_us = timer_unix_time_us();
                    uint64_t plus1d = 86400ULL * 1000000ULL;
                    ok_range = (server_unix_us >= year2000) && (now_wall_us == 0 || server_unix_us <= now_wall_us + plus1d);
                } else {
                    ok_range = (server_unix_us >= year2000);
                }
                if (!ok_range) continue;

                ntp_peer_t* p = NULL;
                for (uint32_t i = 0; i < 2; i++) {
                    if (g_peers[i].ip_host== rip) {
                        p = &g_peers[i];
                        break;
                    }
                }
                if (!p) {
                    for (uint32_t i = 0; i < 2; i++) {
                        if (g_peers[i].ip_host == 0) {
                            memset(&g_peers[i],0, sizeof(g_peers[i]));
                            g_peers[i].ip_host = rip;
                            p = &g_peers[i];
                            break;
                        }
                    }
                }
                if (!p) p = &g_peers[0];

                p->root_delay_us = ntp_short_be_to_us_signed(r->rootDelay);
                p->root_disp_us = ntp_short_be_to_us_signed(r->rootDispersion);

                ntp_sample_t s;
                s.offset_us = off;
                s.delay_us = (uint64_t)rtt;
                s.dispersion_us = (uint64_t)rtt / 2ULL+1000;
                s.mono_time_us = mono_sample_us;

                for (int i = (int)NTP_FILTER_N - 1; i > 0; i--) p->filt[i] = p->filt[i - 1];
                p->filt[0] = s;
                if (p->count < NTP_FILTER_N) p->count++;
                if (p->count > g_max_count) g_max_count = p->count;

                if (p->count) {
                    uint8_t best = 0;
                    uint64_t best_delay = (uint64_t)-1;
                    uint64_t best_disp = (uint64_t)-1;

                    for (uint8_t i = 0; i < p->count; i++) {
                        uint64_t age_us = mono_sample_us -p->filt[i].mono_time_us;
                        uint64_t grow = (age_us * (uint64_t)NTP_PHI_PPM) / 1000000ULL;
                        uint64_t disp = p->filt[i].dispersion_us + grow;
                        uint64_t delay = p->filt[i].delay_us;

                        if (delay < best_delay || (delay == best_delay && disp < best_disp)) {
                            best = i;
                            best_delay = delay;
                            best_disp = disp;
                        }
                    }

                    int64_t best_off = p->filt[best].offset_us;
                    uint64_t sumsq = 0;
                    for (uint8_t i = 0; i < p->count; i++) {
                        int64_t d = p->filt[i].offset_us - best_off;
                        uint64_t a = (uint64_t)abs_i64(d);
                        sumsq += a * a;
                    }

                    uint64_t jitter = 0;
                    jitter = sqrt_u64(sumsq / (uint64_t)p->count);

                    p->best_offset_us = best_off;
                    p->best_delay_us = best_delay;
                    p->best_disp_us = best_disp;
                    p->jitter_us = jitter;

                    uint64_t root_dist = 0;
                    root_dist += p->root_disp_us;
                    root_dist += p->root_delay_us / 2ULL;
                    root_dist += best_delay / 2ULL;
                    root_dist += best_disp;
                    root_dist += jitter;
                    p->root_distance_us = root_dist;
                }

                best_err =NTP_OK;
            }
        } else {
            msleep(50);
            waited += 50;
        }

        if (best_err == NTP_OK && waited >= (timeout_ms / 2)) break;
    }

    socket_destroy_udp(sock);

    ntp_peer_t* best = NULL;
    for (uint32_t i = 0; i < 2; i++) {
        if (g_peers[i].ip_host == 0) continue;
        if (g_peers[i].count == 0) continue;
        if (!best) best = &g_peers[i];
        else {
            uint64_t da = g_peers[i].root_distance_us;
            uint64_t db = best->root_distance_us;
            if (da < db || (da == db && g_peers[i].best_delay_us < best->best_delay_us)) best = &g_peers[i];
        }
    }

    if (!best) return best_err;

    g_best_ip_host = best->ip_host;
    g_best_offset_us = best->best_offset_us;
    g_best_delay_us = best->best_delay_us;
    g_best_disp_us = best->best_disp_us;
    g_best_jitter_us = best->jitter_us;
    g_best_root_distance_us = best->root_distance_us;
    g_best_count = best->count;

    mono_now_us = timer_now_usec();
    uint64_t t4_us = ntp_local_time_us();
    uint64_t server_unix_us = (uint64_t)((int64_t)t4_us + best->best_offset_us);

    discipline_apply(best, server_unix_us, mono_now_us);

    return NTP_OK;
}

uint8_t ntp_max_filter_count(void){ return g_max_count; }