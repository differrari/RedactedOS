#pragma once

#include "../tcp.h"
#include "types.h"
#include "networking/port_manager.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "net/checksums.h"
#include "std/memory.h"
#include "math/rng.h"
#include "syscalls/syscalls.h"
#include "tcp_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCP_REASS_MAX_SEGS 32
#define TCP_REASS_MAX_BYTES (64u * 1024u)
#define TCP_REASS_GLOBAL_MAX_SEGS 1024u
#define TCP_REASS_GLOBAL_MAX_BYTES (2 * 1024u * 1024u)
#define TCP_DEFAULT_MSS 1460
#define TCP_RCV_BUF_MIN (8u * 1024u)
#define TCP_DEFAULT_RCV_BUF (256u * 1024u)
#define TCP_RCV_BUF_MAX (2u * 1024u * 1024u)
#define TCP_TX_MAX_BYTES_PER_FLOW (512u * 1024u)
#define TCP_TX_MAX_BYTES_GLOBAL (4u * 1024u * 1024u)
#define TCP_TX_CONTROL_RESERVE_SEGS 2u
#define TCP_SYN_RECV_MAX_GLOBAL (MAX_TCP_FLOWS / 4u)
#define TCP_SYN_RECV_MAX_LISTENER 32u
#define TCP_SYN_RECV_MAX_SOURCE 8u
#define TCP_TIMEWAIT_MAX_GLOBAL 128u
#define TCP_ORPHAN_MAX_GLOBAL 32u
#define TCP_RESOURCE_BUDGET 4096u
#define TCP_PERSIST_PROBE_BUFSZ 1

#define TCP_SEQ_LT(a,b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) < 0)
#define TCP_SEQ_LEQ(a,b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) <= 0)
#define TCP_SEQ_GT(a,b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) > 0)
#define TCP_SEQ_GEQ(a,b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) >= 0)

#define TCP_DELAYED_ACK_MS 10
#define TCP_PERSIST_MIN_MS 500
#define TCP_PERSIST_MAX_MS 60000
#define TCP_INIT_CWND_SEGS 10
#define TCP_NAGLE_FLUSH_THRESHOLD TCP_DEFAULT_MSS
#define TCP_NAGLE_TIMEOUT_MS 10
#define TCP_CONNECT_TIMEOUT_MS 10000

typedef struct {
    uint8_t used;
    uint8_t syn;
    uint8_t fin;
    uint8_t rtt_sample;
    uint8_t retransmit_cnt;
    uint8_t opts_len;
    uint8_t sacked;
    uint8_t sack_retransmitted;
    uint8_t opts[40];
    uint32_t seq;
    uint64_t len;
    uintptr_t buf;
    uint32_t timer_ms;
    uint32_t timeout_ms;
} tcp_tx_seg_t;

typedef struct {
    uint32_t seq;
    uint32_t end;
} tcp_reass_seg_t;

typedef struct {
    uint16_t local_port;
    uint16_t slot;
    uint16_t active_pos;
    uint32_t generation;
    net_l4_endpoint local;
    net_l4_endpoint remote;
    uint8_t l3_id;
    tcp_state_t state;
    tcp_data ctx;
    uint8_t retries;
} tcp_flow_base_t;

typedef struct {
    uint32_t snd_wnd;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t srtt;
    uint32_t rttvar;
    uint32_t rto;
    uint8_t rtt_valid;

    uint32_t cwnd;
    uint32_t ssthresh;
    uint32_t mss;

    uint8_t ws_send;
    uint8_t ws_recv;
    uint8_t ws_ok;
    uint8_t sack_ok;
    uint8_t dup_acks;
    uint8_t in_fast_recovery;
    uint32_t recover;
    uint32_t cwnd_acc;

    uint8_t nagle_flushing;
    uint8_t nagle_appending;

    uintptr_t nagle_buf;
    uint32_t nagle_len;
    uint32_t nagle_cap;
    uint32_t nagle_timer_ms;

    tcp_tx_seg_t txq[TCP_MAX_TX_SEGS];
    uint32_t queued_bytes;
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
    uint32_t rcv_wnd;
    uint32_t rcv_wnd_max;
    uint32_t rcv_adv_edge;

    tcp_reass_seg_t reass[TCP_REASS_MAX_SEGS];
    uint8_t reass_count;
    uint8_t fin_pending;
    uint32_t fin_seq;
} tcp_flow_rx_t;

typedef struct {
    uint32_t time_wait_ms;
    uint32_t fin_wait2_ms;

    uint8_t persist_active;
    uint8_t persist_probe_cnt;
    uint32_t persist_timer_ms;
    uint32_t persist_timeout_ms;

    uint8_t delayed_ack_pending;
    uint32_t delayed_ack_timer_ms;

    uint8_t keepalive_on;
    uint32_t keepalive_ms;
    uint32_t keepalive_idle_ms;
} tcp_flow_timer_t;

typedef struct {
    uint8_t ttl;
    uint8_t dontfrag;
} tcp_flow_ip_t;

typedef struct {
    tcp_flow_base_t base;
    tcp_flow_tx_t tx;
    tcp_flow_rx_t rx;
    tcp_flow_timer_t timer;
    tcp_flow_ip_t ip;
} tcp_flow_t;

typedef enum {
    TCP_ADMIT_OK = 0,
    TCP_ADMIT_OOO_FLOW_BYTES,
    TCP_ADMIT_OOO_FLOW_SEGS,
    TCP_ADMIT_OOO_GLOBAL_BYTES,
    TCP_ADMIT_OOO_GLOBAL_SEGS,
    TCP_ADMIT_SYN_GLOBAL,
    TCP_ADMIT_SYN_LISTENER,
    TCP_ADMIT_SYN_SOURCE,
    TCP_ADMIT_ORPHAN_LIMIT,
    TCP_ADMIT_TX_FLOW_BYTES,
    TCP_ADMIT_TX_FLOW_SEGS,
    TCP_ADMIT_TX_GLOBAL_BYTES,
    TCP_ADMIT_RESOURCE_BUDGET,
    TCP_ADMIT_FLOW_TABLE_FULL
} tcp_admit_result_t;

typedef struct {
    uint64_t ooo_drop_flow_bytes;
    uint64_t ooo_drop_flow_segs;
    uint64_t ooo_drop_global_bytes;
    uint64_t ooo_drop_global_segs;
    uint64_t syn_drop_global;
    uint64_t syn_drop_listener;
    uint64_t syn_drop_source;
    uint64_t acceptq_drop_full;
    uint64_t timewait_reap_oldest;
    uint64_t orphan_drop_global;
    uint64_t tx_block_flow_bytes;
    uint64_t tx_block_flow_segs;
    uint64_t tx_block_global_bytes;
    uint64_t resource_budget_drop;
    uint64_t flow_table_full;
} tcp_stats_t;

extern tcp_flow_t *tcp_flows[MAX_TCP_FLOWS];
extern uint16_t tcp_active_flows[MAX_TCP_FLOWS];
extern uint16_t tcp_active_count;
extern uint32_t tcp_ooo_global_bytes;
extern uint32_t tcp_ooo_global_segs;
extern uint32_t tcp_tx_global_bytes;
extern tcp_stats_t tcp_stats;

uint32_t tcp_clamp_rcvbuf(uint32_t size);
tcp_admit_result_t tcp_admit_syn(uint8_t l3_id, uint16_t port, ip_version_t ver, const void *src_ip);
tcp_admit_result_t tcp_admit_ooo(tcp_flow_t *flow, uint32_t increase, uint32_t remaining_nodes);
void tcp_account_ooo_remove(uint32_t bytes, uint32_t segs);
void tcp_account_tx_remove(tcp_flow_t *flow, uint32_t bytes);
void tcp_enter_time_wait(tcp_flow_t *flow);

tcp_flow_t *tcp_alloc_flow(void);
void tcp_free_flow(int idx);
void tcp_release_io_buffers(tcp_flow_t *f);
tcp_flow_t *tcp_flow_from_ctx(tcp_data *flow_ctx);

void tcp_rtt_update(tcp_flow_t *flow, uint32_t sample_ms);

tcp_tx_seg_t *tcp_alloc_tx_seg(tcp_flow_t *flow);
bool tcp_send_from_seg(tcp_flow_t *flow, tcp_tx_seg_t *seg);
void tcp_send_ack_now(tcp_flow_t *flow);
int tcp_try_send_pending_fin(tcp_flow_t *flow);
uint64_t tcp_flush_nagle(tcp_flow_t *flow, uint8_t force);

static inline uint16_t tcp_checksum_ipv4(const void *segment, uint16_t seg_len, uint32_t src_ip, uint32_t dst_ip) {
    uint16_t csum = checksum16_pipv4(src_ip, dst_ip, 6, (const uint8_t *)segment, seg_len);
    return bswap16(csum);
}
static inline uint16_t tcp_checksum_ipv6(const void *segment, uint16_t seg_len,  const uint8_t src_ip[16], const uint8_t dst_ip[16]) {
    uint16_t csum = checksum16_pipv6(src_ip, dst_ip, 6, (const uint8_t *)segment, seg_len);
    return bswap16(csum);
}

bool tcp_send_segment(ip_version_t ver, const void *src_ip_addr, const void *dst_ip_addr, tcp_hdr_t *hdr, const uint8_t *opts, uint8_t opts_len, const uint8_t *payload, uint16_t payload_len, const ip_tx_opts_t *txp, uint8_t ttl, uint8_t dontfrag);
void tcp_send_reset(ip_version_t ver, const void *src_ip_addr, const void *dst_ip_addr, uint16_t src_port, uint16_t dst_port, uint32_t seq, uint32_t ack, bool ack_valid);
tcp_tx_seg_t *tcp_find_first_unacked(tcp_flow_t *flow);
void tcp_cc_on_timeout(tcp_flow_t *f);

int tcp_has_pending_timers(void);

void tcp_daemon_kick(void);
uint16_t tcp_calc_adv_wnd_field(tcp_flow_t *flow, uint8_t apply_scale);

#ifdef __cplusplus
}
#endif