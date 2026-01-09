#include "sntp.h" //deprecated, use ntp
#include "exceptions/timer.h"
#include "std/memory.h"
#include "networking/internet_layer/ipv4.h"
#include "process/scheduler.h"
#include "console/kio.h"
#include "types.h"
#include "networking/transport_layer/csocket_udp.h"
#include "syscalls/syscalls.h"
#include "networking/transport_layer/trans_utils.h"

#define NTP_PORT 123
#define NTP_UNIX_EPOCH_DELTA 2208988800UL

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

static sntp_result_t sntp_send_query(socket_handle_t sock, uint32_t server_ip_host, uint64_t* t1_us_out) {
    ntp_packet_t p;
    memset(&p, 0, sizeof(p));
    p.li_vn_mode = (0u<<6) | (4u<<3) | 3u;
    uint64_t t1_us = timer_wall_time_us();
    p.txTs = unix_us_to_ntp64_be(t1_us);
    net_l4_endpoint dst;
    make_ep(server_ip_host, NTP_PORT, IP_VER4, &dst);
    int64_t sent = socket_sendto_udp_ex(sock, DST_ENDPOINT, &dst, 0, &p, sizeof(p));
    if (sent < 0) return SNTP_ERR_SEND;
    *t1_us_out = t1_us;
    return SNTP_OK;
}

sntp_result_t sntp_poll_once(uint32_t timeout_ms){
    uint32_t s0 = 0;
    uint32_t s1 = 0;

    uint8_t l2n = l2_interface_count();
    for (uint8_t i = 0; i < l2n && (s0 == 0 || s1 == 0); i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2) continue;
        for (int s = 0; s < MAX_IPV4_PER_INTERFACE && (s0 == 0 || s1 == 0); s++) {
            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
            if (!v4) continue;
            if (v4->mode == IPV4_CFG_DISABLED) continue;
            const net_runtime_opts_t* rt = &v4->runtime_opts_v4;
            if (!rt) continue;
            uint32_t c0 = rt->ntp[0];
            uint32_t c1 = rt->ntp[1];
            if (c0 && c0 != s0 && c0 != s1){
                if (!s0) s0 = c0;
                else if (!s1) s1 = c0;
            }
            if (c1 && c1 != s0 && c1 != s1){
                if (!s0) s0 = c1;
                else if (!s1) s1 = c1;
            }
        }
    }

    if (s0 == 0 && s1 == 0) return SNTP_ERR_NO_SERVER;

    socket_handle_t sock = udp_socket_create(0, (uint32_t)get_current_proc_pid(), NULL);
    if (sock == 0) return SNTP_ERR_SOCKET;

    uint64_t t1_0 = 0, t1_1 = 0;

    if (s0) {
        if (sntp_send_query(sock, s0, &t1_0) != SNTP_OK) t1_0 = 0;
    }
    if (s1 && s1 != s0) {
        if (sntp_send_query(sock, s1, &t1_1) != SNTP_OK) t1_1 = 0;
    }

    uint32_t waited = 0;
    uint64_t best_server_unix_us = 0;
    uint64_t best_rtt_us = (uint64_t)-1;

    while (waited < timeout_ms){
        uint8_t buf[96];
        net_l4_endpoint src;
        int64_t n = socket_recvfrom_udp_ex(sock, buf, sizeof(buf), &src);

        if (n >= (int64_t)sizeof(ntp_packet_t) && src.ver == IP_VER4 && src.port == NTP_PORT){
            uint32_t rip = 0;
            memcpy(&rip, src.ip, 4);
            if (rip == s0 || rip == s1){
                ntp_packet_t* r = (ntp_packet_t*)buf;
                uint64_t t4_us = timer_wall_time_us();

                uint64_t T1 = ntp64_be_to_unix_us(r->origTs);
                uint64_t T2 = ntp64_be_to_unix_us(r->recvTs);
                uint64_t T3 = ntp64_be_to_unix_us(r->txTs);

                if (T1 != 0 && T3 != 0) {
                    uint64_t t1_us = (rip == s0) ? t1_0 : t1_1;
                    uint64_t d = (T1 > t1_us) ? (T1 - t1_us) : (t1_us - T1);
                    if (t1_us != 0 && d <= 1000000ULL) {
                        int64_t rtt = (int64_t)(t4_us - t1_us) - (int64_t)(T3 - T2);
                        if (rtt < 0) rtt = 0;
                        int64_t off = ((int64_t)(T2 - t1_us) + (int64_t)(T3 - t4_us)) / 2;

                        uint64_t server_unix_us = (uint64_t)((int64_t)t4_us + off);

                        const uint64_t year2000 = 946684800ULL * 1000000ULL;
                        bool ok_range = false;

                        if (timer_is_synchronised()) {
                            uint64_t now_wall_ms = timer_unix_time_ms();
                            uint64_t now_wall_us = now_wall_ms ? (now_wall_ms * 1000ULL) : 0;
                            const uint64_t plus1d = 86400ULL * 1000000ULL;
                            ok_range = (server_unix_us >= year2000) &&(now_wall_us == 0 || server_unix_us <= now_wall_us + plus1d);
                        } else {
                            ok_range = (server_unix_us >= year2000);
                        }
                        if (ok_range) {
                            if ((uint64_t)rtt < best_rtt_us){
                                best_rtt_us = (uint64_t)rtt;
                                best_server_unix_us = server_unix_us;
                            }
                        }
                    }
                }
            }
        } else {
            msleep(50);
            waited += 50;
        }

        if (best_server_unix_us != 0 && waited >= (timeout_ms / 2)) break;
    }

    socket_destroy_udp(sock);

    if (best_server_unix_us == 0) return SNTP_ERR_TIMEOUT;

    //uint64_t sec = best_server_unix_us / 1000000ULL;
    //uint64_t frac = ((best_server_unix_us % 1000000ULL) << 32) / 1000000ULL;
    //uint64_t ntp64 = ((sec + NTP_UNIX_EPOCH_DELTA) << 32) | frac;

    timer_apply_sntp_sample_us(best_server_unix_us);
    return SNTP_OK;
}
