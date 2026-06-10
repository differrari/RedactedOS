#include "tcp_internal.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "std/memory.h"
#include "math/rng.h"
#include "syscalls/syscalls.h"
#include "networking/transport_layer/trans_utils.h"
#include "exceptions/irq.h"

tcp_flow_t *tcp_flows[MAX_TCP_FLOWS];
uint16_t tcp_active_flows[MAX_TCP_FLOWS];
uint16_t tcp_active_count;
static uint32_t tcp_generation_next = 1;

tcp_flow_t *tcp_flow_acquire_match(uint16_t local_port, ip_version_t ver, const void *local_ip, const void *remote_ip, uint16_t remote_port, int *out_idx){
    irq_flags_t irq = irq_save_disable();
    for (uint16_t n = 0; n < tcp_active_count; n++){
        int i = tcp_active_flows[n];
        tcp_flow_t *f = tcp_flows[i];
        if (!f) continue;

        if (f->base.retired || f->base.state == TCP_STATE_CLOSED) continue;
        if (f->base.local_port != local_port) continue;

        uint8_t match = 0;
        if (f->base.state == TCP_LISTEN){
            if (remote_ip || remote_port) continue;
            if (f->base.local.ver && f->base.local.ver != ver) continue;
            if (!local_ip) match = 1;
            else {
                size_t l = (size_t)(ver == IP_VER6 ? 16 : 4);
                uint8_t unspec = 1;
                for (size_t k = 0; k < l; ++k){
                    if (f->base.local.ip[k]){
                        unspec = 0;
                        break;
                    }
                }
                match = unspec || memcmp(f->base.local.ip, local_ip, l) == 0;
            }
        } else {
            if (!remote_ip || !local_ip) continue;
            if (f->base.remote.ver != ver) continue;
            if (f->base.remote.port != remote_port) continue;

            size_t l = (size_t)(ver == IP_VER6 ? 16 : 4);
            if (memcmp(f->base.local.ip, local_ip, l) != 0) continue;
            if (memcmp(f->base.remote.ip, remote_ip, l) != 0) continue;
            match = 1;
        }

        if (!match) continue;
        if (!f->base.refs || f->base.refs == UINT16_MAX) break;

        f->base.refs++;
        if (out_idx) *out_idx = i;
        irq_restore(irq);
        return f;
    }

    if (out_idx) *out_idx = -1;
    irq_restore(irq);
    return NULL;
}

bool tcp_get_ctx(uint16_t local_port, ip_version_t ver, const void *local_ip, const void *remote_ip, uint16_t remote_port, tcp_data *out_ctx){
    if (!out_ctx) return false;
    tcp_flow_t *flow = tcp_flow_acquire_match(local_port, ver, local_ip, remote_ip, remote_port, NULL);
    if (!flow) return false;
    *out_ctx = flow->base.ctx;
    tcp_flow_put(flow);
    return true;
}

int tcp_flow_hold(tcp_flow_t *flow) {
    if (!flow)return 0;

    irq_flags_t irq = irq_save_disable();
    if (flow->base.retired || !flow->base.refs || flow->base.refs == UINT16_MAX) {
        irq_restore(irq);
        return 0;
    }

    flow->base.refs++;
    irq_restore(irq);
    return 1;
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

void tcp_flow_apply_socket_options(tcp_data *flow_ctx, const SocketExtraOptions* extra) {
    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow) return;

    uint32_t flags = extra ? extra->flags : 0;
    flow->timer.keepalive_on = (flags & SOCK_OPT_KEEPALIVE) ? 1 : 0;
    flow->timer.keepalive_ms = flow->timer.keepalive_on ?(extra->keepalive_ms ? extra->keepalive_ms : SOCKET_DEFAULT_KEEPALIVE_MS) : 0;
    flow->timer.keepalive_idle_ms = 0;

    flow->tx.nodelay = (flags & SOCK_OPT_TCP_NO_DELAY) ? 1 : 0;
    if (flow->tx.nodelay && flow->tx.nagle_len) tcp_flush_nagle(flow, 1);

    uint32_t queued_limit = (flags & SOCK_OPT_SEND_BUF_SIZE) && extra->send_buf_size ? extra->send_buf_size : TCP_TX_MAX_BYTES_PER_FLOW;
    if (queued_limit > TCP_TX_MAX_BYTES_PER_FLOW) queued_limit = TCP_TX_MAX_BYTES_PER_FLOW;
    flow->tx.queued_limit = queued_limit;

    flow->ip.ttl = (flags & SOCK_OPT_TTL) ? extra->ttl : 0;
    flow->ip.dontfrag = (flags & SOCK_OPT_DONTFRAG) ? 1 : 0;
    tcp_flow_put(flow);
}

void tcp_flow_put(tcp_flow_t *f) {
    if (!f) return;

    irq_flags_t irq = irq_save_disable();
    if (!f->base.refs) {
        irq_restore(irq);
        return;
    }

    f->base.refs--;
    uint8_t free_now = f->base.refs == 0;
    irq_restore(irq);

    if (!free_now) return;

    for (int i = 0; i < TCP_MAX_TX_SEGS; i++) {
        tcp_tx_seg_t *s = &f->tx.txq[i];
        if (s->buf && s->len) {
            uintptr_t buf = s->buf;
            uint32_t len = (uint32_t)s->len;
            s->buf = 0;
            s->len = 0;
            tcp_account_tx_remove(f, len);
            release((void*)buf);
        }
        memset(s, 0, sizeof(*s));
    }

    f->tx.queued_bytes = 0;

    if (f->tx.nagle_buf) {
        uintptr_t buf = f->tx.nagle_buf;
        f->tx.nagle_buf = 0;
        release((void*)buf);
    }
    f->tx.nagle_len = 0;
    f->tx.nagle_cap = 0;
    f->tx.nagle_timer_ms = 0;
    f->tx.nagle_flushing = 0;
    f->tx.nagle_appending = 0;

    if (f->rx.reass_count || f->rx.rcv_ooo_used) tcp_account_ooo_remove(f->rx.rcv_ooo_used, f->rx.reass_count);
    if (f->rx.rcv_buf) {
        uintptr_t buf = f->rx.rcv_buf;
        f->rx.rcv_buf = 0;
        release((void*)buf);
    }
    f->rx.rcv_base = f->rx.rcv_nxt;
    f->rx.rcv_data_nxt = f->rx.rcv_nxt;
    f->rx.rcv_ooo_used = 0;
    f->rx.rcv_wnd = 0;
    f->rx.rcv_adv_edge = f->rx.rcv_nxt;
    f->rx.fin_pending = 0;
    f->base.ctx.window = 0;
    f->rx.reass_count = 0;
    memset(f->rx.reass, 0, sizeof(f->rx.reass));
    memset(f, 0, sizeof(*f));
    release(f);
}

void tcp_enter_time_wait(tcp_flow_t *flow) {
    if (!flow) return;

    uint32_t timewait = 0;
    int oldest = -1;
    uint32_t oldest_ms = 0;

    irq_flags_t irq = irq_save_disable();
    for (int i = 0; i < MAX_TCP_FLOWS; i++) {
        tcp_flow_t *f = tcp_flows[i];
        if (!f || f->base.retired || f->base.state != TCP_TIME_WAIT) continue;
        timewait++;
        if ((oldest < 0 || f->timer.time_wait_ms > oldest_ms) && f != flow) {
            oldest = i;
            oldest_ms = f->timer.time_wait_ms;
        }
    }
    irq_restore(irq);

    if (timewait >= TCP_TIMEWAIT_MAX_GLOBAL && oldest >= 0) {
        tcp_stats.timewait_reap_oldest++;
        tcp_free_flow(oldest);
    }

    flow->base.state = TCP_TIME_WAIT;
    flow->timer.time_wait_ms = 0;
    tcp_daemon_kick();
}

tcp_flow_t *tcp_alloc_flow(void){
    tcp_flow_t *f = (tcp_flow_t *)zalloc(sizeof(tcp_flow_t));
    if (!f) return NULL;

    irq_flags_t irq = irq_save_disable();
    int slot = -1;
    for (int i = 0; i < MAX_TCP_FLOWS; i++) {
        if (!tcp_flows[i]) {
            slot = i;
            break;
        }
    }

    if (slot < 0 || tcp_active_count >= MAX_TCP_FLOWS) {
        irq_restore(irq);
        release(f);
        return NULL;
    }

    tcp_flows[slot] = f;
    f->base.active_pos = tcp_active_count;
    tcp_active_flows[tcp_active_count++] = (uint16_t)slot;
    f->base.slot = (uint16_t)slot;
    f->base.generation = tcp_generation_next++;
    if (!tcp_generation_next) tcp_generation_next = 1;
    f->base.ctx.flow_index = f->base.slot;
    f->base.ctx.flow_generation = f->base.generation;
    f->base.refs = 1;
    irq_restore(irq);

    f->tx.rto = TCP_INIT_RTO;
    f->rx.rcv_wnd_max = tcp_clamp_rcvbuf(TCP_DEFAULT_RCV_BUF);
    f->rx.rcv_wnd = f->rx.rcv_wnd_max;

    f->tx.mss = TCP_DEFAULT_MSS;
    f->tx.cwnd = f->tx.mss * TCP_INIT_CWND_SEGS;
    f->tx.ssthresh = TCP_RECV_WINDOW;
    return f;
}

void tcp_free_flow(int idx) {
    if (idx < 0 || idx >= MAX_TCP_FLOWS) return;

    irq_flags_t irq = irq_save_disable();
    tcp_flow_t *f = tcp_flows[idx];
    if (!f || f->base.retired) {
        irq_restore(irq);
        return;
    }

    f->base.retired = 1;
    f->base.state = TCP_STATE_CLOSED;
    tcp_flows[idx] = NULL;

    if (tcp_active_count != 0) {
        uint16_t pos = f->base.active_pos;

        if (pos >= tcp_active_count || tcp_active_flows[pos] != idx) for (pos = 0; pos < tcp_active_count && tcp_active_flows[pos] != idx; pos++);
        if (pos < tcp_active_count) {
            uint16_t last = tcp_active_flows[--tcp_active_count];
            tcp_active_flows[pos] = last;
            tcp_active_flows[tcp_active_count] = 0;

            if (last != idx && tcp_flows[last]) tcp_flows[last]->base.active_pos = pos;
        }

        f->base.active_pos = 0;
    }
    irq_restore(irq);

    tcp_flow_put(f);
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
    if (!flow->base.retired && flow->base.state != TCP_STATE_CLOSED && flow->base.state != TCP_TIME_WAIT) {
        tcp_flow_put(flow);
        return TCP_BUSY;
    }
    int slot = (int)flow->base.slot;
    tcp_flow_put(flow);
    tcp_free_flow(slot);
    return TCP_OK;
}

bool tcp_send_segment(ip_version_t ver, const void *src_ip_addr, const void *dst_ip_addr, tcp_hdr_t *hdr, const uint8_t *opts, uint8_t opts_len, const uint8_t *payload, uint16_t payload_len, const ip_tx_opts_t *txp, uint8_t ttl, uint8_t dontfrag){
    if (!hdr) return false;

    if (opts_len & 3u) return false;
    if (opts_len > 40u) return false;

    uint16_t tcp_len = (uint16_t)(sizeof(tcp_hdr_t) + opts_len + payload_len);
    uint32_t headroom = (uint32_t)sizeof(eth_hdr_t) + (uint32_t)(ver == IP_VER4 ? sizeof(ipv4_hdr_t) : sizeof(ipv6_hdr_t));
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
    } else if (ver == IP_VER6){
        h.checksum = tcp_checksum_ipv6(segment, tcp_len, (const uint8_t *)src_ip_addr, (const uint8_t *)dst_ip_addr);
        memcpy(segment, &h, sizeof(h));
        return ipv6_send_packet((const uint8_t *)dst_ip_addr, PROTO_TCP, pkt, txp, ttl, dontfrag);
    }

    netpkt_unref(pkt);
    return false;
}

void tcp_send_reset(ip_version_t ver, const void *src_ip_addr, const void *dst_ip_addr, uint16_t src_port, uint16_t dst_port, uint32_t seq, uint32_t ack, bool ack_valid){
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

    if (ver == IP_VER4){
        ip_tx_opts_t tx;

        tcp_build_tx_opts_from_local_v4(src_ip_addr, &tx);
        tcp_send_segment(IP_VER4, src_ip_addr, dst_ip_addr, &rst_hdr, NULL, 0, NULL, 0, &tx, 0, 0);
    } else if (ver == IP_VER6){
        ip_tx_opts_t tx;

        tcp_build_tx_opts_from_local_v6(src_ip_addr, &tx);
        tcp_send_segment(IP_VER6, src_ip_addr, dst_ip_addr, &rst_hdr, NULL, 0, NULL, 0, &tx, 0, 0);
    }
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

bool tcp_handshake_l3(uint8_t l3_id, uint16_t local_port, net_l4_endpoint *dst, tcp_data *flow_ctx, const SocketExtraOptions* extra){
    if (!dst || !flow_ctx) return false;

    tcp_flow_t *flow = tcp_alloc_flow();
    if (!flow) return false;
    if (!tcp_flow_hold(flow)) {
        tcp_free_flow((int)flow->base.slot);
        return false;
    }

    int idx = (int)flow->base.slot;

    flow->base.local_port = local_port;
    flow->base.l3_id = l3_id;

    flow->base.remote.ver = dst->ver;
    memcpy(flow->base.remote.ip, dst->ip, (size_t)(dst->ver == IP_VER6 ? 16 : 4));
    flow->base.remote.port = dst->port;

    if (dst->ver == IP_VER4){
        l3_ipv4_interface_t *v4 = l3_ipv4_find_by_id(l3_id);

        if (!v4 || !v4->ip){
            tcp_free_flow(idx);
            tcp_flow_put(flow);
            return false;
        }

        make_ep(v4->ip, local_port, IP_VER4, &flow->base.local);
    } else{
        l3_ipv6_interface_t *v6 = l3_ipv6_find_by_id(l3_id);

        if (!v6 || ipv6_is_unspecified(v6->ip)){
            tcp_free_flow(idx);
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
    uint64_t virt_timer;
    asm volatile ("mrs %0, cntvct_el0" : "=r"(virt_timer));
    rng_seed(&rng, virt_timer);
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
    tcp_flow_apply_socket_options(&flow->base.ctx, extra);

    flow->tx.mss = tcp_calc_mss_for_l3(l3_id, dst->ver, dst->ip);

    if (flow->rx.rcv_wnd_max > 65535u) {
        flow->tx.ws_send = 8;
        flow->tx.ws_recv = 0;
        flow->tx.ws_ok = 1;
    } else {
        flow->tx.ws_send = 0;
        flow->tx.ws_recv = 0;
        flow->tx.ws_ok = 0;
    }
    flow->tx.sack_ok = 1;
    flow->rx.rcv_buf = (uintptr_t)zalloc(flow->rx.rcv_wnd_max);
    if (!flow->rx.rcv_buf) {
        flow->rx.rcv_wnd = 0;
        flow->rx.rcv_adv_edge = flow->rx.rcv_nxt;
        flow->base.ctx.window = 0;
        tcp_free_flow(idx);
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

    tcp_tx_seg_t *seg = tcp_alloc_tx_seg(flow);

    if (!seg){
        tcp_free_flow(idx);
        tcp_flow_put(flow);
        return false;
    }

    seg->syn = 1;
    seg->fin = 0;
    seg->rtt_sample = 1;
    seg->retransmit_cnt = 0;
    seg->seq = flow->tx.snd_nxt;
    seg->len = 0;
    seg->buf = 0;
    seg->timer_ms = 0;
    seg->timeout_ms = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;

    seg->opts_len = tcp_build_syn_options(seg->opts, (uint16_t)flow->tx.mss, flow->rx.rcv_wnd_max > 65535u ? flow->tx.ws_send : 0xffu, flow->tx.sack_ok);

    flow->tx.snd_nxt += 1;
    flow->base.ctx.sequence = flow->tx.snd_nxt;
    flow->base.ctx.expected_ack = flow->tx.snd_nxt;

    if (!tcp_send_from_seg(flow, seg)) {
        tcp_free_flow(idx);
        tcp_flow_put(flow);
        return false;
    }

    tcp_daemon_kick();

    uint64_t waited = 0;
    const uint64_t interval = 50;
    const uint64_t max_wait = TCP_CONNECT_TIMEOUT_MS;

    while (waited < max_wait){
        if (flow->base.state == TCP_ESTABLISHED || flow->base.state == TCP_CLOSE_WAIT){
            *flow_ctx = flow->base.ctx;
            tcp_flow_put(flow);
            return true;
        }

        if (flow->base.state == TCP_STATE_CLOSED){
            tcp_free_flow(idx);
            tcp_flow_put(flow);
            return false;
        }

        msleep(interval);
        waited += interval;
    }

    tcp_free_flow(idx);
    tcp_flow_put(flow);
    return false;
}