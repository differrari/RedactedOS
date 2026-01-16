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
#define TCP_DEFAULT_MSS 1460
#define TCP_DEFAULT_RCV_BUF (256u * 1024u)
#define TCP_PERSIST_PROBE_BUFSZ 1

#define TCP_DELAYED_ACK_MS 200
#define TCP_PERSIST_MIN_MS 500
#define TCP_PERSIST_MAX_MS 60000

typedef struct {
    uint8_t used;
    uint8_t syn;
    uint8_t fin;
    uint8_t rtt_sample;
    uint8_t retransmit_cnt;
    uint32_t seq;
    uint64_t len;
    uintptr_t buf;
    uint32_t timer_ms;
    uint32_t timeout_ms;
} tcp_tx_seg_t;

typedef struct {
    uint32_t seq;
    uint32_t end;
    uintptr_t buf;
} tcp_reass_seg_t;

typedef struct {
    uint16_t local_port;
    net_l4_endpoint local;
    net_l4_endpoint remote;
    uint8_t l3_id;
    tcp_state_t state;
    tcp_data ctx;
    uint8_t retries;
    uint32_t snd_wnd;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t srtt;
    uint32_t rttvar;
    uint32_t rto;
    uint8_t rtt_valid;
    uint32_t time_wait_ms;
    uint32_t fin_wait2_ms;

    uint32_t rcv_nxt;
    uint32_t rcv_buf_used;
    uint32_t rcv_wnd;
    uint32_t rcv_wnd_max;
    uint32_t rcv_adv_edge;

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

    uint8_t persist_active;
    uint8_t persist_probe_cnt;
    uint32_t persist_timer_ms;
    uint32_t persist_timeout_ms;

    uint8_t delayed_ack_pending;
    uint32_t delayed_ack_timer_ms;

    tcp_reass_seg_t reass[TCP_REASS_MAX_SEGS];
    uint8_t reass_count;
    tcp_tx_seg_t txq[TCP_MAX_TX_SEGS];
    uint8_t fin_pending;
    uint32_t fin_seq;

    uint8_t ip_ttl;
    uint8_t ip_dontfrag;
    uint8_t keepalive_on;
    uint32_t keepalive_ms;
    uint32_t keepalive_idle_ms;
} tcp_flow_t;

extern tcp_flow_t *tcp_flows[MAX_TCP_FLOWS];

tcp_flow_t *tcp_alloc_flow(void);
void tcp_free_flow(int idx);

void tcp_rtt_update(tcp_flow_t *flow, uint32_t sample_ms);

tcp_tx_seg_t *tcp_alloc_tx_seg(tcp_flow_t *flow);
void tcp_send_from_seg(tcp_flow_t *flow, tcp_tx_seg_t *seg);
void tcp_send_ack_now(tcp_flow_t *flow);

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