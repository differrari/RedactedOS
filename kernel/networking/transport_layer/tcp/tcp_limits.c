#include "tcp_internal.h"
#include "exceptions/irq.h"

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
    if (increase && (flow->rx.rcv_ooo_used > limit || increase > limit - flow->rx.rcv_ooo_used)) return TCP_ADMIT_OOO_FLOW_BYTES;
    if (increase && (tcp_ooo_global_bytes > TCP_REASS_GLOBAL_MAX_BYTES || increase > TCP_REASS_GLOBAL_MAX_BYTES - tcp_ooo_global_bytes)) return TCP_ADMIT_OOO_GLOBAL_BYTES;
    if (seg_increase && tcp_ooo_global_segs + seg_increase > TCP_REASS_GLOBAL_MAX_SEGS) return TCP_ADMIT_OOO_GLOBAL_SEGS;

    return TCP_ADMIT_OK;
}

tcp_admit_result_t tcp_admit_syn(struct ksocket* listener, ip_version_t ver, const void *src_ip) {
    if (!listener) return TCP_ADMIT_SYN_LISTENER;
    uint32_t syn_total = 0;
    uint32_t syn_listener = 0;
    uint32_t syn_source = 0;
    size_t ip_len = (size_t)(ver == IP_VER6 ? 16 : 4);

    irq_flags_t irq = irq_save_disable();
    uint32_t active_count = tcp_active_count;
    for (uint16_t n = 0; n < active_count; n++) {
        uint16_t slot = tcp_active_flows[n];
        tcp_flow_t* flow = slot < MAX_TCP_FLOWS ? tcp_flows[slot] : NULL;
        if (!flow || flow->base.retired || flow->base.state != TCP_SYN_RECEIVED) continue;
        syn_total++;
        if (flow->base.listener != listener) continue;
        syn_listener++;
        if (src_ip && flow->base.remote.ver == ver && memcmp(flow->base.remote.ip, src_ip, ip_len) == 0) syn_source++;
    }
    irq_restore(irq);

    if (active_count >= MAX_TCP_FLOWS) return TCP_ADMIT_FLOW_TABLE_FULL;

    uint32_t available = MAX_TCP_FLOWS - active_count;
    if (available <= TCP_FLOW_CONTROL_RESERVE) return TCP_ADMIT_FLOW_RESERVE;

    uint32_t usable = available - TCP_FLOW_CONTROL_RESERVE;
    uint32_t global_limit = usable / 2;
    if (global_limit < TCP_SYN_RECV_MIN_GLOBAL) global_limit = TCP_SYN_RECV_MIN_GLOBAL;
    if (global_limit > TCP_SYN_RECV_MAX_GLOBAL) global_limit = TCP_SYN_RECV_MAX_GLOBAL;
    if (global_limit > usable) global_limit = usable;

    uint32_t listener_limit = usable / 8;
    if (listener_limit < TCP_SYN_RECV_MIN_LISTENER) listener_limit = TCP_SYN_RECV_MIN_LISTENER;
    if (listener_limit > TCP_SYN_RECV_MAX_LISTENER) listener_limit = TCP_SYN_RECV_MAX_LISTENER;
    if (listener_limit > global_limit) listener_limit = global_limit;

    uint32_t source_limit = listener_limit / 4;
    if (source_limit < TCP_SYN_RECV_MIN_SOURCE) source_limit = TCP_SYN_RECV_MIN_SOURCE;
    if (source_limit > TCP_SYN_RECV_MAX_SOURCE) source_limit = TCP_SYN_RECV_MAX_SOURCE;
    if (source_limit > listener_limit) source_limit = listener_limit;

    if (syn_total >= global_limit) return TCP_ADMIT_SYN_GLOBAL;
    if (syn_listener >= listener_limit) return TCP_ADMIT_SYN_LISTENER;
    if (syn_source >= source_limit) return TCP_ADMIT_SYN_SOURCE;

    return TCP_ADMIT_OK;
}

tcp_admit_result_t tcp_admit_tx(tcp_flow_t *flow, uint32_t bytes, uint32_t free_slots) {
    if (!flow) return TCP_ADMIT_TX_FLOW_BYTES;
    if (free_slots <= TCP_TX_CONTROL_RESERVE_SEGS) return TCP_ADMIT_TX_FLOW_SEGS;
    uint32_t limit = flow->tx.queued_limit ? flow->tx.queued_limit : TCP_TX_MAX_BYTES_PER_FLOW;
    if (flow->tx.queued_bytes >= limit) return TCP_ADMIT_TX_FLOW_BYTES;
    if (tcp_tx_global_bytes >= TCP_TX_MAX_BYTES_GLOBAL) return TCP_ADMIT_TX_GLOBAL_BYTES;
    if (bytes && (flow->tx.queued_bytes > limit || bytes > limit - flow->tx.queued_bytes)) return TCP_ADMIT_TX_FLOW_BYTES;
    if (bytes && (tcp_tx_global_bytes > TCP_TX_MAX_BYTES_GLOBAL || bytes > TCP_TX_MAX_BYTES_GLOBAL - tcp_tx_global_bytes)) return TCP_ADMIT_TX_GLOBAL_BYTES;
    return TCP_ADMIT_OK;
}
