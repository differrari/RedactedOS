#pragma once

#include "../tcp.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCP_REASS_MAX_SEGS 32
#define TCP_REASS_MAX_BYTES (64u * 1024u)
#define TCP_REASS_GLOBAL_MAX_SEGS 1024u
#define TCP_REASS_GLOBAL_MAX_BYTES (2u * 1024u * 1024u)
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
#define TCP_FLOW_CONTROL_RESERVE 32u
#define TCP_SYN_RECV_MIN_GLOBAL 16u
#define TCP_SYN_RECV_MIN_LISTENER 4u
#define TCP_SYN_RECV_MIN_SOURCE 2u

struct tcp_flow;
struct ksocket;

typedef enum {
    TCP_ADMIT_OK = 0,
    TCP_ADMIT_OOO_FLOW_BYTES,
    TCP_ADMIT_OOO_FLOW_SEGS,
    TCP_ADMIT_OOO_GLOBAL_BYTES,
    TCP_ADMIT_OOO_GLOBAL_SEGS,
    TCP_ADMIT_SYN_GLOBAL,
    TCP_ADMIT_SYN_LISTENER, 
    TCP_ADMIT_SYN_SOURCE,
    TCP_ADMIT_FLOW_RESERVE,
    TCP_ADMIT_TX_FLOW_BYTES,
    TCP_ADMIT_TX_FLOW_SEGS,
    TCP_ADMIT_TX_GLOBAL_BYTES,
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
    uint64_t syn_drop_flow_reserve;
    uint64_t tx_block_flow_bytes;
    uint64_t tx_block_flow_segs;
    uint64_t tx_block_global_bytes;
    uint64_t flow_table_full;
} tcp_stats_t;

extern uint32_t tcp_ooo_global_bytes;
extern uint32_t tcp_ooo_global_segs;
extern uint32_t tcp_tx_global_bytes;
extern tcp_stats_t tcp_stats;

uint32_t tcp_clamp_rcvbuf(uint32_t size);
tcp_admit_result_t tcp_admit_syn(struct ksocket *listener, ip_version_t ver, const void *src_ip);
tcp_admit_result_t tcp_admit_ooo(struct tcp_flow *flow, uint32_t increase, uint32_t remaining_nodes);
tcp_admit_result_t tcp_admit_tx(struct tcp_flow *flow, uint32_t bytes, uint32_t free_slots);
void tcp_account_ooo_add(uint32_t bytes, uint32_t segs);
void tcp_account_ooo_remove(uint32_t bytes, uint32_t segs);
void tcp_account_tx_add(struct tcp_flow *flow, uint32_t bytes);
void tcp_account_tx_remove(struct tcp_flow *flow, uint32_t bytes);

#ifdef __cplusplus
}
#endif
