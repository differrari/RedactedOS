#include "tcp_internal.h"
#include "std/memory.h"

uint32_t tcp_ooo_global_bytes;
uint32_t tcp_ooo_global_segs;
uint32_t tcp_tx_global_bytes;
tcp_stats_t tcp_stats;

uint32_t tcp_clamp_rcvbuf(uint32_t size) {
    if (!size) size = TCP_DEFAULT_RCV_BUF;
    if (size < TCP_RCV_BUF_MIN) size = TCP_RCV_BUF_MIN;
    if (size > TCP_RCV_BUF_MAX) size = TCP_RCV_BUF_MAX;
    return size;
}

void tcp_account_ooo_add(uint32_t bytes, uint32_t segs) {
    tcp_ooo_global_bytes += bytes;
    tcp_ooo_global_segs += segs;
}

void tcp_account_ooo_remove(uint32_t bytes, uint32_t segs) {
    if (tcp_ooo_global_bytes >= bytes) tcp_ooo_global_bytes -= bytes;
    else tcp_ooo_global_bytes = 0;
    if (tcp_ooo_global_segs >= segs) tcp_ooo_global_segs -= segs;
    else tcp_ooo_global_segs = 0;
}

void tcp_account_tx_add(tcp_flow_t *flow, uint32_t bytes) {
    if (!flow || !bytes) return;
    flow->tx.queued_bytes += bytes;
    tcp_tx_global_bytes += bytes;
}

void tcp_account_tx_remove(tcp_flow_t *flow, uint32_t bytes) {
    if (!flow || !bytes) return;
    if (flow->tx.queued_bytes >= bytes) flow->tx.queued_bytes -= bytes;
    else flow->tx.queued_bytes = 0;
    if (tcp_tx_global_bytes >= bytes) tcp_tx_global_bytes -= bytes;
    else tcp_tx_global_bytes = 0;
}

tcp_admit_result_t tcp_admit_ooo(tcp_flow_t *flow, uint32_t increase, uint32_t remaining_nodes) {
    if (!flow) return TCP_ADMIT_OOO_FLOW_BYTES;

    uint32_t mss = flow->tx.mss ? flow->tx.mss : TCP_DEFAULT_MSS;
    uint32_t limit = flow->rx.rcv_wnd_max >> 1;
    uint32_t floor = mss * 4u;

    if (floor > TCP_REASS_MAX_BYTES / 2u) floor = TCP_REASS_MAX_BYTES / 2u;
    if (limit > TCP_REASS_MAX_BYTES) limit = TCP_REASS_MAX_BYTES;
    if (limit < floor) limit = floor;
    if (limit > flow->rx.rcv_wnd_max) limit = flow->rx.rcv_wnd_max;

    uint32_t seg_increase = remaining_nodes >= flow->rx.reass_count ? 1u : 0;

    if (remaining_nodes >= TCP_REASS_MAX_SEGS) return TCP_ADMIT_OOO_FLOW_SEGS;
    if (increase && flow->rx.rcv_ooo_used + increase > limit) return TCP_ADMIT_OOO_FLOW_BYTES;
    if (increase && tcp_ooo_global_bytes + increase > TCP_REASS_GLOBAL_MAX_BYTES) return TCP_ADMIT_OOO_GLOBAL_BYTES;
    if (seg_increase && tcp_ooo_global_segs + seg_increase > TCP_REASS_GLOBAL_MAX_SEGS) return TCP_ADMIT_OOO_GLOBAL_SEGS;

    return TCP_ADMIT_OK;
}

tcp_admit_result_t tcp_admit_syn(uint8_t l3_id, uint16_t port, ip_version_t ver, const void *src_ip) {
    uint32_t syn_total = 0;
    uint32_t syn_listener = 0;
    uint32_t syn_source = 0;
    uint32_t timewait = 0;
    uint32_t orphan = 0;
    size_t ip_len = (size_t)(ver == IP_VER6 ? 16 : 4);

    for (int i = 0; i < MAX_TCP_FLOWS; i++) {
        tcp_flow_t *f = tcp_flows[i];
        if (!f) continue;

        if (f->base.state == TCP_SYN_RECEIVED) {
            syn_total++;
            if (f->base.local_port == port && f->base.l3_id == l3_id) syn_listener++;
            if (src_ip && f->base.local_port == port && f->base.l3_id == l3_id && f->base.remote.ver == ver && memcmp(f->base.remote.ip, src_ip, ip_len) == 0) syn_source++;
        } else if (f->base.state == TCP_TIME_WAIT) timewait++;
        else if (f->base.state != TCP_LISTEN && f->base.state != TCP_STATE_CLOSED && !f->base.local_port) orphan++;
    }

    uint32_t score = (uint32_t)tcp_active_count * 4 + syn_total * 4 + timewait + orphan * 4 + (tcp_ooo_global_bytes >> 12) + (tcp_tx_global_bytes >> 12);

    if (tcp_active_count >= MAX_TCP_FLOWS) return TCP_ADMIT_FLOW_TABLE_FULL;
    if (syn_total >= TCP_SYN_RECV_MAX_GLOBAL) return TCP_ADMIT_SYN_GLOBAL;
    if (syn_listener >= TCP_SYN_RECV_MAX_LISTENER) return TCP_ADMIT_SYN_LISTENER;
    if (syn_source >= TCP_SYN_RECV_MAX_SOURCE) return TCP_ADMIT_SYN_SOURCE;
    if (orphan >= TCP_ORPHAN_MAX_GLOBAL) return TCP_ADMIT_ORPHAN_LIMIT;
    if (score >= TCP_RESOURCE_BUDGET) return TCP_ADMIT_RESOURCE_BUDGET;

    return TCP_ADMIT_OK;
}

tcp_admit_result_t tcp_admit_tx(tcp_flow_t *flow, uint32_t bytes, uint32_t free_slots) {
    if (!flow) return TCP_ADMIT_TX_FLOW_BYTES;
    if (free_slots <= TCP_TX_CONTROL_RESERVE_SEGS) return TCP_ADMIT_TX_FLOW_SEGS;
    uint32_t limit = flow->tx.queued_limit ? flow->tx.queued_limit : TCP_TX_MAX_BYTES_PER_FLOW;
    if (flow->tx.queued_bytes >= limit) return TCP_ADMIT_TX_FLOW_BYTES;
    if (tcp_tx_global_bytes >= TCP_TX_MAX_BYTES_GLOBAL) return TCP_ADMIT_TX_GLOBAL_BYTES;
    if (bytes && flow->tx.queued_bytes + bytes > limit) return TCP_ADMIT_TX_FLOW_BYTES;
    if (bytes && tcp_tx_global_bytes + bytes > TCP_TX_MAX_BYTES_GLOBAL) return TCP_ADMIT_TX_GLOBAL_BYTES;
    return TCP_ADMIT_OK;
}
