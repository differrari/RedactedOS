#pragma once

#include "../tcp.h"
#include "types.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "net/checksums.h"
#include "std/memory.h"
#include "math/rng.h"
#include "syscalls/syscalls.h"
#include "tcp_utils.h"
#include "tcp_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCP_DEFAULT_MSS 1460
#define TCP_DEFAULT_PEER_MSS_IPV4 536
#define TCP_DEFAULT_PEER_MSS_IPV6 1220
#define TCP_MAX_MSS (NETPKT_MAX_ALLOC - sizeof(ipv4_hdr_t) - sizeof(tcp_hdr_t))

#define TCP_SEQ_LT(a,b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) < 0)
#define TCP_SEQ_LEQ(a,b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) <= 0)
#define TCP_SEQ_GT(a,b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) > 0)
#define TCP_SEQ_GEQ(a,b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) >= 0)

#define TCP_DELAYED_ACK_MS 10
#define TCP_PERSIST_MIN_MS 500
#define TCP_PERSIST_MAX_MS 60000
#define TCP_NAGLE_FLUSH_THRESHOLD TCP_DEFAULT_MSS
#define TCP_NAGLE_TIMEOUT_MS 10
#define TCP_CONNECT_TIMEOUT_MS 10000
#define TCP_SACK_SCOREBOARD_MAX 32
#define TCP_CHALLENGE_ACK_INTERVAL_MS 100

typedef struct {
    uint32_t left;
    uint32_t right;
} tcp_sack_range_t;

typedef struct {
    uint8_t used;
    uint8_t syn;
    uint8_t fin;
    uint8_t psh;
    uint8_t persist;
    uint8_t rtt_sample;
    uint8_t retransmit_cnt;
    uint8_t opts_len;
    uint8_t opts[40];
    uint32_t seq;
    uint32_t len;
    netpkt_t *pkt;
    uint32_t payload_off;
    uint32_t timer_ms;
    uint32_t rtt_timer_ms;
    uint32_t timeout_ms;
} tcp_tx_seg_t;

typedef struct {
    uint32_t seq;
    uint32_t end;
} tcp_reass_seg_t;

typedef struct {
    uint16_t slot;
    uint16_t active_pos;
    uint32_t generation;
    net_l4_endpoint local;
    net_l4_endpoint remote;
    l3_id_t l3_id;
    uint32_t l3_generation;
    tcp_state_t state;
    tcp_data ctx;
    uint16_t refs;
    uint8_t retired;
    uint8_t active_open;
    struct ksocket *listener;
} tcp_flow_base_t;

typedef struct {
    uint32_t snd_wnd;
    uint32_t snd_wl1;
    uint32_t snd_wl2;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t srtt;
    uint32_t rttvar;
    uint32_t rto;
    uint8_t rtt_valid;
    uint8_t rtt_sample_pending;

    uint32_t cwnd;
    uint32_t ssthresh;
    uint32_t mss;
    uint32_t advertised_mss;
    uint32_t path_mss;
    uint32_t peer_mss;

    uint8_t ws_send;
    uint8_t ws_recv;
    uint8_t ws_ok;
    uint8_t sack_ok;
    uint8_t dup_acks;
    uint8_t in_fast_recovery;
    uint8_t recover_valid;
    uint32_t recover;
    uint32_t cwnd_acc;
    
    uint32_t configured_mss;
    uint8_t sack_enabled;
    uint8_t dsack_enabled;
    tcp_sack_range_t sack_ranges[TCP_SACK_SCOREBOARD_MAX];
    tcp_sack_range_t sack_retransmitted_ranges[TCP_SACK_SCOREBOARD_MAX];
    uint8_t sack_range_count;
    uint8_t sack_retransmitted_count;
    uint32_t sack_rescue_rxt;
    uint8_t sack_rescue_valid;

    uint8_t nagle_flushing;
    uint8_t nagle_appending;
    uint8_t nodelay;
    uint8_t nagle_psh;
    uint8_t data_tx_valid;

    uintptr_t nagle_buf;
    uint32_t nagle_len;
    uint32_t nagle_cap;
    uint32_t nagle_timer_ms;
    uint32_t last_data_tx_ms;

    tcp_tx_seg_t txq[TCP_MAX_TX_SEGS];
    uint32_t queued_bytes;
    uint32_t queued_limit;
    uint8_t fin_tx_pending;
} tcp_flow_tx_t;

typedef struct {
    uint32_t rcv_nxt;
    uint32_t rcv_base;
    uint32_t rcv_data_nxt;
    uintptr_t rcv_buf;
    uint32_t rcv_ooo_used;
    uint32_t sack_recent_left;
    uint32_t sack_recent_right;
    uint8_t dsack_pending;
    uint8_t urg_valid;
    uint32_t dsack_left;
    uint32_t dsack_right;
    uint32_t rcv_wnd;
    uint32_t rcv_wnd_max;
    uint32_t rcv_adv_edge;

    tcp_reass_seg_t reass[TCP_REASS_MAX_SEGS];
    uint8_t reass_count;
    uint8_t fin_pending;
    uint32_t fin_seq;
    uint32_t urg_seq;
} tcp_flow_rx_t;

typedef struct {
    uint32_t time_wait_ms;
    uint32_t fin_wait2_ms;

    uint8_t persist_active;
    uint32_t persist_timer_ms;
    uint32_t persist_timeout_ms;

    uint8_t delayed_ack_pending;
    uint32_t delayed_ack_timer_ms;

    uint8_t keepalive_on;
    uint32_t keepalive_ms;
    uint32_t keepalive_idle_ms;

    uint8_t challenge_ack_valid;
    uint32_t challenge_ack_last_ms;
} tcp_flow_timer_t;

typedef struct {
    uint8_t ttl;
    uint8_t dontfrag;
    uint8_t reuseaddr;
    uint8_t dontroute;
} tcp_flow_ip_t;

typedef struct tcp_flow {
    tcp_flow_base_t base;
    tcp_flow_tx_t tx;
    tcp_flow_rx_t rx;
    tcp_flow_timer_t timer;
    tcp_flow_ip_t ip;
} tcp_flow_t;
//TODO rfc 6675 is not complete
extern tcp_flow_t *tcp_flows[MAX_TCP_FLOWS];
extern uint16_t tcp_active_flows[MAX_TCP_FLOWS];
extern uint16_t tcp_active_count;

void tcp_enter_time_wait(tcp_flow_t *flow);

tcp_flow_t *tcp_alloc_flow(void);
void tcp_free_flow(tcp_flow_t *flow);
bool tcp_active_insert_flow(tcp_flow_t *flow);
tcp_flow_t *tcp_flow_from_ctx(tcp_data *flow_ctx);
tcp_flow_t *tcp_flow_acquire_match(uint16_t local_port, ip_version_t ver, const void *local_ip, const void *remote_ip, uint16_t remote_port);
void tcp_flow_put(tcp_flow_t *flow);
void tcp_flow_apply_options(tcp_flow_t *flow, const SocketOptions* extra, uint32_t apply_mask);

void tcp_rtt_update(tcp_flow_t *flow, uint32_t sample_ms);

tcp_tx_seg_t *tcp_alloc_tx_seg(tcp_flow_t *flow, uint32_t reserve_slots);
const uint8_t *tcp_tx_seg_payload_ptr(const tcp_tx_seg_t *seg);
void tcp_tx_seg_clear(tcp_flow_t *flow, tcp_tx_seg_t *seg);
bool tcp_send_from_seg(tcp_flow_t *flow, tcp_tx_seg_t *seg);
bool tcp_retransmit_seg(tcp_flow_t *flow, tcp_tx_seg_t *seg);
bool tcp_send_flow_segment(tcp_flow_t *flow, tcp_hdr_t *hdr, const uint8_t *opts, uint8_t opts_len, const uint8_t *payload, uint16_t payload_len);
void tcp_send_ack_now(tcp_flow_t *flow);
void tcp_try_send_pending_fin(tcp_flow_t *flow);
uint64_t tcp_flush_nagle(tcp_flow_t *flow, uint8_t force);

static inline uint16_t tcp_checksum_ipv4(const void *segment, uint16_t seg_len, uint32_t src_ip, uint32_t dst_ip) {
    uint16_t csum = checksum16_pipv4(src_ip, dst_ip, PROTO_TCP, (const uint8_t *)segment, seg_len);
    return bswap16(csum);
}
static inline uint16_t tcp_checksum_ipv6(const void *segment, uint16_t seg_len,  const uint8_t src_ip[16], const uint8_t dst_ip[16]) {
    uint16_t csum = checksum16_pipv6(src_ip, dst_ip, PROTO_TCP, (const uint8_t *)segment, seg_len);
    return bswap16(csum);
}

bool tcp_send_segment(ip_version_t ver, const void *src_ip_addr, const void *dst_ip_addr, tcp_hdr_t *hdr, const uint8_t *opts, uint8_t opts_len, const uint8_t *payload, uint16_t payload_len, const ip_tx_opts_t *txp, uint8_t ttl, uint8_t dontfrag, uint8_t dontroute);
void tcp_send_reset(l3_id_t l3_id, ip_version_t ver, const void *src_ip_addr, const void *dst_ip_addr, uint16_t src_port, uint16_t dst_port, uint32_t seq, uint32_t ack, bool ack_valid);
tcp_tx_seg_t *tcp_find_first_unacked(tcp_flow_t *flow);
void tcp_cc_on_timeout(tcp_flow_t *f);

void tcp_daemon_kick(void);
void tcp_update_adv_wnd(tcp_flow_t *flow, uint8_t apply_scale);

#ifdef __cplusplus
}
#endif