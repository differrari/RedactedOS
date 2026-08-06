#include "tcp_internal.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "std/memory.h"
#include "math/rng.h"
#include "random/random.h"
#include "syscalls/syscalls.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/transport_layer/socket_core.h"
#include "networking/transport_layer/socket_bind.h"
#include "exceptions/irq.h"

tcp_flow_t *tcp_flows[MAX_TCP_FLOWS];
uint16_t tcp_active_flows[MAX_TCP_FLOWS];
uint16_t tcp_active_count;
static uint32_t tcp_generation_next = 1;
static uint16_t tcp_alloc_cursor;

static uint16_t tcp_active_port_lower_bound(uint16_t local_port) {
    uint16_t lo = 0;
    uint16_t hi = tcp_active_count;

    while (lo < hi) { 
        uint16_t mid = lo + ((hi - lo) >> 1);
        tcp_flow_t *flow = tcp_flows[tcp_active_flows[mid]];
        if (flow->base.local.port < local_port) lo = (uint16_t)(mid + 1);
        else hi = mid;
    }

    return lo;
}

static bool tcp_flow_tuple_matches(const tcp_flow_t *flow, uint16_t local_port, ip_version_t ver, const void *local_ip, const void *remote_ip, uint16_t remote_port) {
    if (!flow || !local_ip || !remote_ip || (ver != IP_VER4 && ver != IP_VER6)) return false;
    if (flow->base.local.port != local_port || flow->base.remote.port != remote_port) return false;
    if (flow->base.local.ver != ver || flow->base.remote.ver != ver) return false;

    size_t ip_len = ver == IP_VER6 ? 16 : 4;
    if (memcmp(flow->base.local.ip, local_ip, ip_len) != 0) return false;
    if (memcmp(flow->base.remote.ip, remote_ip, ip_len) != 0) return false;
    return true;
}

bool tcp_active_insert_flow(tcp_flow_t *flow) {
    if (!flow) return false;

    irq_flags_t irq = irq_save_disable();
    uint16_t slot = flow->base.slot;
    if (slot >= MAX_TCP_FLOWS || tcp_flows[slot] != flow || flow->base.retired || flow->base.active_pos != UINT16_MAX || tcp_active_count >= MAX_TCP_FLOWS) {
        irq_restore(irq);
        return false;
    }

    uint16_t local_port = flow->base.local.port;
    uint16_t pos = tcp_active_port_lower_bound(local_port);
    for (uint16_t n = pos; n < tcp_active_count; ++n) {
        uint16_t cur_slot = tcp_active_flows[n];
        tcp_flow_t *cur = cur_slot < MAX_TCP_FLOWS ? tcp_flows[cur_slot] : NULL;
        if (!cur) continue;
        if (cur->base.local.port > local_port) break;
        if (cur->base.retired || cur->base.state == TCP_STATE_CLOSED) continue;
        if (tcp_flow_tuple_matches(cur, local_port, flow->base.local.ver, flow->base.local.ip, flow->base.remote.ip, flow->base.remote.port)) {
            irq_restore(irq);
            return false;
        }
    }

    while (pos < tcp_active_count) {
        uint16_t cur_slot = tcp_active_flows[pos];
        tcp_flow_t *cur = cur_slot < MAX_TCP_FLOWS ? tcp_flows[cur_slot] : NULL;
        uint16_t cur_port = cur ? cur->base.local.port : UINT16_MAX;
        if (cur_port != local_port || cur_slot > slot) break;
        pos++;
    }

    for (uint16_t i = tcp_active_count; i > pos; i--) {
        uint16_t moved_slot = tcp_active_flows[i-1];
        tcp_active_flows[i] = moved_slot;
        if (moved_slot < MAX_TCP_FLOWS && tcp_flows[moved_slot]) tcp_flows[moved_slot]->base.active_pos = i;
    }

    tcp_active_flows[pos] = slot;
    flow->base.active_pos = pos;
    tcp_active_count++;
    irq_restore(irq);
    return true;
}

tcp_flow_t *tcp_flow_acquire_match(uint16_t local_port, ip_version_t ver, const void *local_ip, const void *remote_ip, uint16_t remote_port){
    irq_flags_t irq = irq_save_disable();
    uint16_t start = tcp_active_port_lower_bound(local_port);
    for (uint16_t n = start; n < tcp_active_count; n++){
        uint16_t slot = tcp_active_flows[n];
        tcp_flow_t *f = slot < MAX_TCP_FLOWS ? tcp_flows[slot] : NULL;
        if (!f) continue;

        if (f->base.local.port > local_port) break;
        if (f->base.retired || f->base.state == TCP_STATE_CLOSED) continue;
        if (f->base.local.port != local_port) continue;

        if (!tcp_flow_tuple_matches(f, local_port, ver, local_ip, remote_ip, remote_port)) continue;
        if (!f->base.refs || f->base.refs == UINT16_MAX) break;

        f->base.refs++;
        irq_restore(irq);
        return f;
    }

    irq_restore(irq);
    return NULL;
}

bool tcp_bind_conflicts(const SockBindSpec* spec, uint16_t port, bool reuseaddr) {
    if (!spec || !port) return true;

    bool conflict = false;
    irq_flags_t irq = irq_save_disable();
    uint16_t start = tcp_active_port_lower_bound(port);
    for (uint16_t n = start; n < tcp_active_count; n++) {
        uint16_t slot = tcp_active_flows[n];
        tcp_flow_t* flow = slot < MAX_TCP_FLOWS ? tcp_flows[slot] : NULL;
        if (!flow) continue;
        if (flow->base.local.port > port) break;
        if (flow->base.retired || flow->base.state == TCP_STATE_CLOSED) continue;
        uint8_t ifindex = l3_ifindex_from_id(flow->base.l3_id);
        if (flow->base.local.port != port || !socket_bind_match_score(spec, flow->base.local.ver, flow->base.l3_id, ifindex, flow->base.local.ip)) continue;

        bool allowed = reuseaddr && flow->ip.reuseaddr;
        if (!allowed) {
            conflict = true;
            break;
        }
    }
    irq_restore(irq);
    return conflict;
}

bool tcp_get_ctx(uint16_t local_port, ip_version_t ver, const void *local_ip, const void *remote_ip, uint16_t remote_port, tcp_data *out_ctx){
    if (!out_ctx) return false;
    tcp_flow_t *flow = tcp_flow_acquire_match(local_port, ver, local_ip, remote_ip, remote_port);
    if (!flow) return false;
    *out_ctx = flow->base.ctx;
    tcp_flow_put(flow);
    return true;
}

tcp_flow_t *tcp_flow_from_ctx(tcp_data *flow_ctx) {
    if (!flow_ctx) return NULL;
    if (flow_ctx->flow_index >= MAX_TCP_FLOWS) return NULL;

    irq_flags_t irq = irq_save_disable();
    tcp_flow_t *flow = tcp_flows[flow_ctx->flow_index];
    if (!flow || flow->base.retired || flow->base.generation != flow_ctx->flow_generation || !flow->base.refs || flow->base.refs == UINT16_MAX) {
        irq_restore(irq);
        return NULL;
    }

    flow->base.refs++;
    irq_restore(irq);
    return flow;
}

void tcp_flow_apply_options(tcp_flow_t *flow, const SocketOptions* extra, uint32_t apply_mask) {
    if (!flow) return;
    SocketOptions defaults = {0};
    defaults.flags = SOCK_OPT_TCP_SACK | SOCK_OPT_TCP_DSACK;
    if (!extra) extra = &defaults;
    uint32_t flags = extra->flags;

    if (apply_mask & (SOCK_OPT_KEEPALIVE | SOCK_OPT_KEEPALIVE_INTERVAL)) {
        flow->timer.keepalive_on = (flags & SOCK_OPT_KEEPALIVE) ? 1 : 0;
        flow->timer.keepalive_ms = flow->timer.keepalive_on ? (extra->keepalive_ms ? extra->keepalive_ms : SOCKET_DEFAULT_KEEPALIVE_MS) : 0;
        flow->timer.keepalive_idle_ms = 0;
        if (flow->base.active_pos != UINT16_MAX) tcp_daemon_kick();
    }

    if (apply_mask & SOCK_OPT_TCP_NO_DELAY) {
        flow->tx.nodelay = (flags & SOCK_OPT_TCP_NO_DELAY) ? 1 : 0;
        if (flow->tx.nodelay && flow->tx.nagle_len) tcp_flush_nagle(flow, 1);
    }

    if (apply_mask & SOCK_OPT_SEND_BUF_SIZE) {
        uint32_t queued_limit = (flags & SOCK_OPT_SEND_BUF_SIZE) && extra->send_buf_size ? extra->send_buf_size : TCP_TX_MAX_BYTES_PER_FLOW;
        if (queued_limit > TCP_TX_MAX_BYTES_PER_FLOW) queued_limit = TCP_TX_MAX_BYTES_PER_FLOW;
        flow->tx.queued_limit = queued_limit;
    }

    if (apply_mask & SOCK_OPT_TCP_MAXSEG) {
        flow->tx.configured_mss = (flags & SOCK_OPT_TCP_MAXSEG) && extra->tcp_maxseg ? extra->tcp_maxseg : 0;
        tcp_update_mss(flow);
    }

    if (apply_mask & (SOCK_OPT_TCP_SACK | SOCK_OPT_TCP_DSACK)) {
        flow->tx.sack_enabled = (flags & SOCK_OPT_TCP_SACK) ? 1 : 0;
        flow->tx.dsack_enabled = flow->tx.sack_enabled && (flags & SOCK_OPT_TCP_DSACK) ? 1 : 0;
        if (!flow->tx.sack_enabled) {
            flow->tx.sack_ok = 0;
            flow->tx.sack_range_count = 0;
            flow->tx.sack_retransmitted_count = 0;
            flow->tx.sack_rescue_sent = 0;
            flow->rx.dsack_pending = 0;
            flow->rx.dsack_left = 0;
            flow->rx.dsack_right = 0;
        }
    }

    if (apply_mask & SOCK_OPT_TTL) flow->ip.ttl = (flags & SOCK_OPT_TTL) ? extra->ttl : 0;
    if (apply_mask & SOCK_OPT_DONTFRAG) flow->ip.dontfrag = (flags & SOCK_OPT_DONTFRAG) ? 1 : 0;
    if (apply_mask & SOCK_OPT_REUSEADDR) flow->ip.reuseaddr = (flags & SOCK_OPT_REUSEADDR) ? 1 : 0;
}

void tcp_flow_apply_socket_options(tcp_data *flow_ctx, const SocketOptions* extra, uint32_t apply_mask) {
    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow) return;
    tcp_flow_apply_options(flow, extra, apply_mask);
    tcp_flow_put(flow);
}

static void tcp_flow_clear_storage(tcp_flow_t *flow) {
    for (uint32_t i = 0; i < TCP_MAX_TX_SEGS; i++) tcp_tx_seg_clear(flow, &flow->tx.txq[i]);
    flow->tx.queued_bytes = 0;

    if (flow->tx.nagle_buf) {
        uintptr_t buf = flow->tx.nagle_buf;
        flow->tx.nagle_buf = 0;
        release((void*)buf);
    }
    flow->tx.nagle_len = 0;
    flow->tx.nagle_cap = 0;
    flow->tx.nagle_timer_ms = 0;
    flow->tx.nagle_flushing = 0;
    flow->tx.nagle_appending = 0;

    if (flow->rx.reass_count || flow->rx.rcv_ooo_used) tcp_account_ooo_remove(flow->rx.rcv_ooo_used, flow->rx.reass_count);
    if (flow->rx.rcv_buf) {
        uintptr_t buf = flow->rx.rcv_buf;
        flow->rx.rcv_buf = 0;
        release((void*)buf);
    }
    flow->rx.rcv_base = flow->rx.rcv_nxt;
    flow->rx.rcv_data_nxt = flow->rx.rcv_nxt;
    flow->rx.rcv_ooo_used = 0;
    flow->rx.rcv_wnd = 0;
    flow->rx.rcv_adv_edge = flow->rx.rcv_nxt;
    flow->rx.fin_pending = 0;
    flow->base.ctx.window = 0;
    flow->rx.reass_count = 0;
    memset(flow->rx.reass, 0, sizeof(flow->rx.reass));
    if (flow->base.listener) {
        socket_core_put(flow->base.listener);
        flow->base.listener = NULL;
    }
}

void tcp_flow_put(tcp_flow_t *f) {
    if (!f) return;

    irq_flags_t irq = irq_save_disable();
    if (!f->base.refs) {
        irq_restore(irq);
        return;
    }

    f->base.refs--;
    bool free_now = f->base.refs == 0;
    irq_restore(irq);

    if (!free_now) return;

    tcp_flow_clear_storage(f);
    memset(f, 0, sizeof(*f));
    release(f);
}

void tcp_enter_time_wait(tcp_flow_t *flow) {
    if (!flow) return;

    uint32_t timewait = 0;
    tcp_flow_t *oldest = NULL;
    uint32_t oldest_ms = 0;

    irq_flags_t irq = irq_save_disable();
    for (uint16_t n = 0; n < tcp_active_count; n++) {
        uint16_t slot = tcp_active_flows[n];
        tcp_flow_t *f = slot < MAX_TCP_FLOWS ? tcp_flows[slot] : NULL;
        if (!f || f->base.retired || f->base.state != TCP_TIME_WAIT) continue;
        timewait++;
        if ((!oldest || f->timer.time_wait_ms > oldest_ms) && f != flow) {
            oldest = f;
            oldest_ms = f->timer.time_wait_ms;
        }
    }
    if (timewait >= TCP_TIMEWAIT_MAX_GLOBAL && oldest && oldest->base.refs && oldest->base.refs != UINT16_MAX) oldest->base.refs++;
    else oldest = NULL;
    irq_restore(irq);

    if (timewait >= TCP_TIMEWAIT_MAX_GLOBAL && oldest) {
        tcp_stats.timewait_reap_oldest++;
        tcp_free_flow(oldest);
        tcp_flow_put(oldest);
    }

    flow->base.state = TCP_TIME_WAIT;
    flow->timer.time_wait_ms = 0;
    tcp_flow_clear_storage(flow);
    tcp_daemon_kick();
}

tcp_flow_t *tcp_alloc_flow(void){
    tcp_flow_t *f = (tcp_flow_t *)zalloc(sizeof(tcp_flow_t));
    if (!f) return NULL;

    irq_flags_t irq = irq_save_disable();
    int32_t slot = -1;
    for (uint32_t n = 0; n < MAX_TCP_FLOWS; n++) {
        uint16_t i = (uint16_t)((tcp_alloc_cursor + n) % MAX_TCP_FLOWS);
        if (!tcp_flows[i]) {
            slot = i;
            tcp_alloc_cursor = (uint16_t)((i + 1) % MAX_TCP_FLOWS);
            break;
        }
    }

    if (slot < 0 || tcp_active_count >= MAX_TCP_FLOWS) {
        irq_restore(irq);
        release(f);
        return NULL;
    }

    tcp_flows[slot] = f;
    f->base.slot = (uint16_t)slot;
    f->base.active_pos = UINT16_MAX;
    f->base.generation = tcp_generation_next++;
    if (!tcp_generation_next) tcp_generation_next = 1;
    f->base.ctx.flow_index = f->base.slot;
    f->base.ctx.flow_generation = f->base.generation;
    f->base.refs = 2;
    irq_restore(irq);

    f->tx.rto = TCP_INIT_RTO;
    f->rx.rcv_wnd_max = tcp_clamp_rcvbuf(TCP_DEFAULT_RCV_BUF);
    f->rx.rcv_wnd = f->rx.rcv_wnd_max;

    f->tx.path_mss = TCP_DEFAULT_MSS;
    f->tx.peer_mss = 0;
    f->tx.advertised_mss = TCP_DEFAULT_MSS;
    f->tx.mss = TCP_DEFAULT_MSS;
    f->tx.sack_enabled = 1;
    f->tx.dsack_enabled = 1;
    f->tx.cwnd = f->tx.mss * TCP_INIT_CWND_SEGS;
    f->tx.ssthresh = TCP_RECV_WINDOW;
    return f;
}

void tcp_free_flow(tcp_flow_t *flow) {
    if (!flow) return;
    irq_flags_t irq = irq_save_disable();
    uint16_t slot = flow->base.slot;
    if (slot >= MAX_TCP_FLOWS || tcp_flows[slot] != flow || flow->base.retired) {
        irq_restore(irq);
        return;
    }

    flow->base.retired = 1;
    flow->base.state = TCP_STATE_CLOSED;

    if (tcp_active_count != 0 && flow->base.active_pos != UINT16_MAX) {
        uint16_t pos = flow->base.active_pos;

        if (pos >= tcp_active_count || tcp_active_flows[pos] != slot) {
            pos = 0;
            while (pos < tcp_active_count && tcp_active_flows[pos] != slot) pos++;
        }

        if (pos < tcp_active_count) {
            for (uint16_t i = pos; i + 1 < tcp_active_count; i++) {
                uint16_t moved = tcp_active_flows[i+1];
                tcp_active_flows[i] = moved;
                if (moved < MAX_TCP_FLOWS && tcp_flows[moved]) tcp_flows[moved]->base.active_pos = i;
            }
            tcp_active_count--;
            tcp_active_flows[tcp_active_count] = 0;
        }

        flow->base.active_pos = UINT16_MAX;
    }

    tcp_flows[slot] = NULL;
    if (slot < tcp_alloc_cursor) tcp_alloc_cursor = slot;
    irq_restore(irq);

    tcp_flow_put(flow);
}

tcp_result_t tcp_flow_abort(tcp_data *flow_ctx) {
    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow) return TCP_INVALID;

    tcp_state_t state = flow->base.state;
    if (state != TCP_STATE_CLOSED && state != TCP_TIME_WAIT && state != TCP_SYN_SENT) tcp_send_reset(flow->base.l3_id, flow->base.local.ver, flow->base.local.ip, flow->base.remote.ip, flow->base.local.port, flow->base.remote.port, flow->tx.snd_nxt, 0, false);

    tcp_free_flow(flow);
    tcp_flow_put(flow);
    return TCP_OK;
}

bool tcp_flow_is_closed(tcp_data *flow_ctx) {
    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow) return true;
    bool closed = flow->base.state == TCP_STATE_CLOSED || flow->base.state == TCP_TIME_WAIT;
    tcp_flow_put(flow);
    return closed;
}

tcp_result_t tcp_flow_release_closed(tcp_data *flow_ctx) {
    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow) return TCP_INVALID;
    if (flow->base.state == TCP_TIME_WAIT) {
        tcp_flow_put(flow);
        return TCP_OK;
    }
    if (flow->base.state != TCP_STATE_CLOSED) {
        tcp_flow_put(flow);
        return TCP_BUSY;
    }
    tcp_free_flow(flow);
    tcp_flow_put(flow);
    return TCP_OK;
}

bool tcp_send_segment(ip_version_t ver, const void *src_ip_addr, const void *dst_ip_addr, tcp_hdr_t *hdr, const uint8_t *opts, uint8_t opts_len, const uint8_t *payload, uint16_t payload_len, const ip_tx_opts_t *txp, uint8_t ttl, uint8_t dontfrag){
    if (!hdr || !src_ip_addr || !dst_ip_addr) return false;
    if (ver != IP_VER4 && ver != IP_VER6) return false;

    if (opts_len & 3u) return false;
    if (opts_len > 40u) return false;
    if ((opts_len && !opts) || (payload_len && !payload)) return false;

    uint32_t ip_header_len = ver == IP_VER4 ? (uint32_t)sizeof(ipv4_hdr_t) : (uint32_t)sizeof(ipv6_hdr_t);
    uint32_t max_tcp_len = ver == IP_VER4 ? UINT16_MAX - ip_header_len : UINT16_MAX;
    uint32_t tcp_len32 = (uint32_t)sizeof(tcp_hdr_t) + (uint32_t)opts_len + (uint32_t)payload_len;
    if (tcp_len32 > max_tcp_len || tcp_len32 > NETPKT_MAX_ALLOC) return false;
    uint16_t tcp_len = (uint16_t)tcp_len32;
    uint32_t headroom = (uint32_t)sizeof(eth_hdr_t) + ip_header_len;
    netpkt_t *pkt = netpkt_alloc(tcp_len, headroom, 0);
    if (!pkt) return false;
    uint8_t *segment = (uint8_t*)netpkt_put(pkt, tcp_len);
    if (!segment) {
        netpkt_unref(pkt);
        return false;
    }

    tcp_hdr_t h = *hdr;

    uint8_t header_words = (uint8_t)((sizeof(tcp_hdr_t) + opts_len) / 4);
    h.data_offset_reserved = (uint8_t)(header_words << 4);
    h.window = bswap16(h.window);
    h.checksum = 0;

    memcpy(segment, &h, sizeof(tcp_hdr_t));
    if (opts_len && opts) memcpy(segment + sizeof(tcp_hdr_t), opts, opts_len);
    if (payload_len && payload) memcpy(segment + sizeof(tcp_hdr_t) + opts_len, payload, payload_len);

    if (ver == IP_VER4){
        uint32_t s = 0;
        uint32_t d = 0;
        memcpy(&s, src_ip_addr, sizeof(s));
        memcpy(&d, dst_ip_addr, sizeof(d));

        h.checksum = tcp_checksum_ipv4(segment, tcp_len, s, d);
        memcpy(segment, &h, sizeof(h));
        return ipv4_send_packet(d, PROTO_TCP, pkt, txp, ttl, dontfrag);
    }

    h.checksum = tcp_checksum_ipv6(segment, tcp_len, (const uint8_t *)src_ip_addr, (const uint8_t *)dst_ip_addr);
    memcpy(segment, &h, sizeof(h));
    return ipv6_send_packet((const uint8_t *)dst_ip_addr, PROTO_TCP, pkt, txp, ttl, dontfrag);
}

void tcp_send_reset(uint8_t l3_id, ip_version_t ver, const void *src_ip_addr, const void *dst_ip_addr, uint16_t src_port, uint16_t dst_port, uint32_t seq, uint32_t ack, bool ack_valid){
    if (!l3_id) return;
    tcp_hdr_t rst_hdr;

    rst_hdr.src_port = bswap16(src_port);
    rst_hdr.dst_port = bswap16(dst_port);

    if (ack_valid){
        rst_hdr.sequence = bswap32(0);
        rst_hdr.ack = bswap32(ack);
        rst_hdr.flags = (uint8_t)((1u << RST_F) | (1u << ACK_F));
    } else{
        rst_hdr.sequence = bswap32(seq);
        rst_hdr.ack = bswap32(0);
        rst_hdr.flags = (uint8_t)(1u << RST_F);
    }

    rst_hdr.window = 0;
    rst_hdr.urgent_ptr = 0;

    ip_tx_opts_t tx;
    tx.scope = IP_TX_BOUND_L3;
    tx.index = l3_id;
    (void)tcp_send_segment(ver, src_ip_addr, dst_ip_addr, &rst_hdr, NULL, 0, NULL, 0, &tx, 0, 0);
}

void tcp_rtt_update(tcp_flow_t *flow, uint32_t sample_ms){
    if (sample_ms == 0) sample_ms = 1;

    if (!flow->tx.rtt_valid){
        flow->tx.srtt = sample_ms;
        flow->tx.rttvar = sample_ms / 2;

        uint32_t rto = flow->tx.srtt + (flow->tx.rttvar << 2);
        if (rto < TCP_MIN_RTO) rto = TCP_MIN_RTO;
        if (rto > TCP_MAX_RTO) rto = TCP_MAX_RTO;

        flow->tx.rto = rto;
        flow->tx.rtt_valid = 1;

        return;
    }

    uint32_t srtt = flow->tx.srtt;
    uint32_t rttvar = flow->tx.rttvar;

    uint32_t diff = srtt > sample_ms ? srtt - sample_ms : sample_ms - srtt;
    uint32_t new_rttvar = (uint32_t)((3 * (uint64_t)rttvar + (uint64_t)diff) >> 2);
    uint32_t new_srtt = (uint32_t)(((uint64_t)7 * srtt + sample_ms) >> 3);

    flow->tx.srtt = new_srtt;
    flow->tx.rttvar = new_rttvar;

    uint32_t rto = new_srtt + (new_rttvar << 2);
    if (rto < TCP_MIN_RTO) rto = TCP_MIN_RTO;
    if (rto > TCP_MAX_RTO) rto = TCP_MAX_RTO;

    flow->tx.rto = rto;
}

bool tcp_handshake_l3(uint8_t l3_id, uint16_t local_port, net_l4_endpoint *dst, tcp_data *flow_ctx, const SocketOptions* extra){
    if (!dst || !flow_ctx) return false;

    tcp_flow_t *flow = tcp_alloc_flow();
    if (!flow) return false;
    flow->base.l3_id = l3_id;

    flow->base.remote.ver = dst->ver;
    memcpy(flow->base.remote.ip, dst->ip, (size_t)(dst->ver == IP_VER6 ? 16 : 4));
    flow->base.remote.port = dst->port;

    if (dst->ver == IP_VER4){
        l3_ipv4_interface_t *v4 = l3_ipv4_find_by_id(l3_id);

        if (!v4 || !v4->ip){
            tcp_free_flow(flow);
            tcp_flow_put(flow);
            return false;
        }

        make_ep(&v4->ip, local_port, IP_VER4, &flow->base.local);
    } else{
        l3_ipv6_interface_t *v6 = l3_ipv6_find_by_id(l3_id);

        if (!v6 || ipv6_is_unspecified(v6->ip)){
            tcp_free_flow(flow);
            tcp_flow_put(flow);
            return false;
        }

        flow->base.local.ver = IP_VER6;
        memset(flow->base.local.ip, 0, sizeof(flow->base.local.ip));
        memcpy(flow->base.local.ip, v6->ip, sizeof(flow->base.local.ip));
        flow->base.local.port = local_port;
    }

    flow->base.state = TCP_SYN_SENT;
    flow->base.retries = TCP_SYN_RETRIES;

    rng_t rng;
    rng_init_random(&rng);
    uint32_t iss = rng_next32(&rng);

    flow->base.ctx.sequence = iss;
    flow->base.ctx.ack = 0;

    flow->rx.rcv_nxt = 0;
    flow->rx.rcv_base = 0;
    flow->rx.rcv_data_nxt = 0;
    flow->rx.rcv_ooo_used = 0;
    flow->rx.sack_recent_left = 0;
    flow->rx.sack_recent_right = 0;
    uint32_t rcvbuf = TCP_DEFAULT_RCV_BUF;
    if (extra && (extra->flags & SOCK_OPT_BUF_SIZE) && extra->buf_size) rcvbuf = extra->buf_size;
    flow->rx.rcv_wnd_max = tcp_clamp_rcvbuf(rcvbuf);
    flow->rx.rcv_adv_edge = 0;
    flow->tx.path_mss = tcp_calc_mss_for_l3(l3_id, dst->ver, dst->ip);
    flow->tx.peer_mss = 0;
    tcp_flow_apply_options(flow, extra, UINT32_MAX);

    if (flow->rx.rcv_wnd_max > 65535u) {
        flow->tx.ws_send = 8;
        flow->tx.ws_recv = 0;
        flow->tx.ws_ok = 1;
    } else {
        flow->tx.ws_send = 0;
        flow->tx.ws_recv = 0;
        flow->tx.ws_ok = 0;
    }
    flow->tx.sack_ok = flow->tx.sack_enabled;
    flow->rx.rcv_buf = (uintptr_t)zalloc(flow->rx.rcv_wnd_max);
    if (!flow->rx.rcv_buf) {
        flow->rx.rcv_wnd = 0;
        flow->rx.rcv_adv_edge = flow->rx.rcv_nxt;
        flow->base.ctx.window = 0;
        tcp_free_flow(flow);
        tcp_flow_put(flow);
        return false;
    }
    (void)tcp_calc_adv_wnd_field(flow, 1);

    flow->base.ctx.options.ptr = 0;
    flow->base.ctx.options.size = 0;
    flow->base.ctx.payload.ptr = 0;
    flow->base.ctx.payload.size = 0;

    flow->base.ctx.flags = (uint8_t)(1u << SYN_F);
    flow->base.ctx.expected_ack = iss + 1;
    flow->base.ctx.ack_received = 0;

    flow->tx.snd_una = iss;
    flow->tx.snd_nxt = iss;
    flow->tx.snd_wnd = 0;

    flow->tx.cwnd = flow->tx.mss * TCP_INIT_CWND_SEGS;
    flow->tx.ssthresh = TCP_RECV_WINDOW;
    flow->tx.dup_acks = 0;
    flow->tx.in_fast_recovery = 0;
    flow->tx.recover = 0;
    flow->tx.cwnd_acc = 0;

    flow->timer.time_wait_ms = 0;
    flow->timer.fin_wait2_ms = 0;

    tcp_tx_seg_t *seg = tcp_alloc_tx_seg(flow, 0);

    if (!seg){
        tcp_free_flow(flow);
        tcp_flow_put(flow);
        return false;
    }

    seg->syn = 1;
    seg->fin = 0;
    seg->rtt_sample = 1;
    seg->retransmit_cnt = 0;
    seg->seq = flow->tx.snd_nxt;
    seg->len = 0;
    seg->pkt = NULL;
    seg->payload_off = 0;
    seg->timer_ms = 0;
    seg->timeout_ms = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;

    seg->opts_len = tcp_build_syn_options(seg->opts, (uint16_t)flow->tx.advertised_mss, flow->rx.rcv_wnd_max > 65535u ? flow->tx.ws_send : 0xffu, flow->tx.sack_ok);

    flow->tx.snd_nxt += 1;
    flow->base.ctx.sequence = flow->tx.snd_nxt;
    flow->base.ctx.expected_ack = flow->tx.snd_nxt;

    if (!tcp_active_insert_flow(flow) || !tcp_send_from_seg(flow, seg)) {
        tcp_free_flow(flow);
        tcp_flow_put(flow);
        return false;
    }

    *flow_ctx = flow->base.ctx;
    tcp_flow_put(flow);
    return true;
}