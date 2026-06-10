#include "tcp_internal.h"
#include "networking/transport_layer/socket_bind.h"
#include "networking/transport_layer/csocket_tcp.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "std/memory.h"
#include "math/rng.h"
#include "syscalls/syscalls.h"
#include "../tcp.h"

static int16_t tcp_reass_count_ok(tcp_flow_t *flow) {
    if (!flow) return 0;

    if (flow->rx.reass_count <= TCP_REASS_MAX_SEGS) return 1;

    if (flow->rx.reass_count || flow->rx.rcv_ooo_used) tcp_account_ooo_remove(flow->rx.rcv_ooo_used, flow->rx.reass_count);
    flow->rx.reass_count = 0;
    flow->rx.rcv_ooo_used = 0;
    flow->rx.sack_recent_left = 0;
    flow->rx.sack_recent_right = 0;

    for (int i = 0; i < TCP_REASS_MAX_SEGS; i++) {
        flow->rx.reass[i].seq = 0;
        flow->rx.reass[i].end = 0;
    }

    return 0;
}

static void tcp_reass_remove(tcp_flow_t *flow, int idx) {
    if (!tcp_reass_count_ok(flow)) return;
    if (idx < 0 || idx >= flow->rx.reass_count) return;

    uint32_t len = 0;
    if (TCP_SEQ_GT(flow->rx.reass[idx].end, flow->rx.reass[idx].seq)) len = flow->rx.reass[idx].end - flow->rx.reass[idx].seq;

    if (flow->rx.rcv_ooo_used >= len) flow->rx.rcv_ooo_used -= len;
    else flow->rx.rcv_ooo_used = 0;
    tcp_account_ooo_remove(len, 1);

    for (int i = idx; i + 1 < flow->rx.reass_count; i++) flow->rx.reass[i] = flow->rx.reass[i+1];

    if (flow->rx.reass_count) flow->rx.reass_count--;
    flow->rx.reass[flow->rx.reass_count].seq = 0;
    flow->rx.reass[flow->rx.reass_count].end = 0;
}

static int tcp_reass_drain_inseq(tcp_flow_t *flow) {
    int advanced = 0;
    if (!tcp_reass_count_ok(flow)) return 0;

    for(;;){
        int idx = -1;

        for (int i = 0; i < flow->rx.reass_count; i++){
            if (flow->rx.reass[i].seq != flow->rx.rcv_nxt) continue;
            idx = i;
            break;
        }

        if (idx < 0) break;

        uint32_t end = flow->rx.reass[idx].end;
        if (TCP_SEQ_LEQ(end, flow->rx.rcv_nxt)) {
            tcp_reass_remove(flow, idx);
            continue;
        }

        flow->rx.rcv_nxt = end;
        flow->rx.rcv_data_nxt = end;
        flow->base.ctx.ack = flow->rx.rcv_nxt;
        tcp_reass_remove(flow, idx);
        advanced = 1;
    }

    if (advanced) tcp_calc_adv_wnd_field(flow, 1);
    return advanced;
}

int64_t tcp_flow_read(tcp_data *flow_ctx, void *buf, uint64_t len) {
    if (!buf || !len) return 0;

    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow) return TCP_DISCONNECT;

    int64_t rc = 0;
    if (!flow->rx.rcv_buf || !flow->rx.rcv_wnd_max) rc = flow->base.state == TCP_STATE_CLOSED ? TCP_DISCONNECT : 0;
    else if (TCP_SEQ_LT(flow->rx.rcv_data_nxt, flow->rx.rcv_base)) rc = flow->base.state == TCP_STATE_CLOSED ? TCP_DISCONNECT : 0;
    else {
        uint32_t n = flow->rx.rcv_data_nxt - flow->rx.rcv_base;
        if (!n) rc = flow->base.state == TCP_STATE_CLOSED ? TCP_DISCONNECT : 0;
        else {
            if (len < n) n = (uint32_t)len;

            uint8_t *rx = (uint8_t*)flow->rx.rcv_buf;
            uint8_t *dst = (uint8_t*)buf;
            uint32_t pos = flow->rx.rcv_base % flow->rx.rcv_wnd_max;
            uint32_t first = flow->rx.rcv_wnd_max - pos;

            if (first > n) first = n;
            if (first) memcpy(dst, rx + pos, first);
            if (n > first) memcpy(dst + first, rx, n - first);

            flow->rx.rcv_base += n;
            rc = n;
        }
    }
    
    tcp_flow_put(flow);
    if (rc > 0) tcp_flow_on_app_read(flow_ctx, (uint32_t)rc);
    return rc;
}

uint32_t tcp_flow_readable(tcp_data *flow_ctx) {
    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow) return 0;

    uint32_t n = 0;
    if (flow->rx.rcv_buf && flow->rx.rcv_wnd_max && !TCP_SEQ_LT(flow->rx.rcv_data_nxt, flow->rx.rcv_base)) n = flow->rx.rcv_data_nxt - flow->rx.rcv_base;

    tcp_flow_put(flow);
    return n;
}

bool tcp_flow_recv_closed(tcp_data *flow_ctx) {
    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow) return true;

    bool closed = false;
    if (flow->base.state == TCP_STATE_CLOSED || flow->base.state == TCP_TIME_WAIT) closed = true;
    else if (flow->base.state == TCP_CLOSE_WAIT) {
        if (!flow->rx.rcv_buf || !flow->rx.rcv_wnd_max) closed = true;
        else if (TCP_SEQ_LT(flow->rx.rcv_data_nxt, flow->rx.rcv_base)) closed = true;
        else closed = flow->rx.rcv_data_nxt == flow->rx.rcv_base;
    }

    tcp_flow_put(flow);
    return closed;
}

tcp_tx_seg_t *tcp_find_first_unacked(tcp_flow_t *flow) {
    tcp_tx_seg_t *best = NULL;
    uint32_t best_seq = 0;

    for (int i = 0; i < TCP_MAX_TX_SEGS; i++){
        tcp_tx_seg_t *s = &flow->tx.txq[i];

        if (!s->used) continue;

        uint32_t end = s->seq + s->len + (s->syn ? 1u : 0u) + (s->fin ? 1u : 0u);
        if (TCP_SEQ_LEQ(end, flow->tx.snd_una)) continue;

        if (!best || TCP_SEQ_LT(s->seq, best_seq)){
            best = s;
            best_seq = s->seq;
        }
    }

    return best;
}

static int tcp_apply_sack_blocks(tcp_flow_t *flow, const tcp_parsed_opts_t *opts) {
    if (!flow || !opts || !flow->tx.sack_ok || !opts->sack_count) return 0;

    int changed = 0;
    for (uint32_t b = 0; b < opts->sack_count; b++) {
        uint32_t left = opts->sacks[b].left;
        uint32_t right = opts->sacks[b].right;

        if (TCP_SEQ_LEQ(right, left)) continue;
        if (TCP_SEQ_LEQ(right, flow->tx.snd_una)) continue;
        if (TCP_SEQ_GEQ(left, flow->tx.snd_nxt)) continue;

        for (int i = 0; i < TCP_MAX_TX_SEGS; i++) {
            tcp_tx_seg_t *s = &flow->tx.txq[i];

            if (!s->used) continue;

            uint32_t end = s->seq + s->len + (s->syn ? 1 : 0) + (s->fin ? 1 : 0);
            if (TCP_SEQ_LEQ(end, flow->tx.snd_una)) continue;
            if (TCP_SEQ_LT(s->seq, left) || TCP_SEQ_GT(end, right)) continue;

            if (!s->sacked) changed = 1;
            s->sacked = 1;
        }
    }

    return changed;
}

static tcp_tx_seg_t *tcp_find_sack_retransmit(tcp_flow_t *flow) {
    tcp_tx_seg_t *best = NULL;
    uint32_t best_seq = 0;
    uint32_t highest_sacked = flow->tx.snd_una;

    for (int i = 0; i < TCP_MAX_TX_SEGS; i++) {
        tcp_tx_seg_t *s = &flow->tx.txq[i];

        if (!s->used || !s->sacked) continue;

        uint32_t end = s->seq + s->len + (s->syn ? 1u : 0u) + (s->fin ? 1u : 0u);
        if (TCP_SEQ_LEQ(end, flow->tx.snd_una)) continue;
        if (TCP_SEQ_GT(end, highest_sacked)) highest_sacked = end;
    }

    if (highest_sacked == flow->tx.snd_una) return NULL;

    for (int i = 0; i < TCP_MAX_TX_SEGS; i++) {
        tcp_tx_seg_t *s = &flow->tx.txq[i];

        if (!s->used)continue;
        if (s->sacked || s->sack_retransmitted) continue;

        uint32_t end = s->seq + s->len + (s->syn ? 1 : 0) + (s->fin ? 1 : 0);
        if (TCP_SEQ_LEQ(end, flow->tx.snd_una)) continue;
        if (TCP_SEQ_GEQ(s->seq, highest_sacked)) continue;

        if (!best || TCP_SEQ_LT(s->seq, best_seq)) {
            best = s;
            best_seq = s->seq;
        }
    }

    return best;
}

void tcp_cc_on_timeout(tcp_flow_t *f){
    uint32_t mss = f->tx.mss ? f->tx.mss : TCP_DEFAULT_MSS;
    uint32_t flight = TCP_SEQ_GT(f->tx.snd_nxt, f->tx.snd_una) ? f->tx.snd_nxt - f->tx.snd_una : 0;
    uint32_t half = flight / 2;
    uint32_t minth = 2u * mss;

    if (half < minth) half = minth;

    f->tx.ssthresh = half;
    f->tx.cwnd = mss;
    f->tx.cwnd_acc = 0;
    f->tx.dup_acks = 0;
    f->tx.in_fast_recovery = 0;
    f->tx.recover = 0;

    for (int i = 0; i < TCP_MAX_TX_SEGS; i++) {
        f->tx.txq[i].sacked = 0;
        f->tx.txq[i].sack_retransmitted = 0;
    }
}

static void tcp_cc_on_new_ack(tcp_flow_t *f, uint32_t ack) {
    uint32_t mss = f->tx.mss ? f->tx.mss : TCP_DEFAULT_MSS;

    if (f->tx.in_fast_recovery){
        if (TCP_SEQ_GEQ(ack, f->tx.recover)){
            f->tx.cwnd = f->tx.ssthresh;
            if (f->tx.cwnd < mss) f->tx.cwnd = mss;

            f->tx.in_fast_recovery = 0;
            f->tx.dup_acks = 0;
            f->tx.cwnd_acc = 0;
            return;
        }

        f->tx.cwnd = f->tx.ssthresh;
        if (f->tx.cwnd < mss) f->tx.cwnd = mss;
        return;
    }

    if (f->tx.cwnd < f->tx.ssthresh){
        f->tx.cwnd += mss;
        if (f->tx.cwnd < mss) f->tx.cwnd = mss;
        return;
    }

    uint32_t denom = f->tx.cwnd ? f->tx.cwnd : 1u;
    uint32_t inc = (mss * mss) / denom;

    if (inc == 0) inc = 1;

    f->tx.cwnd += inc;
}

static void tcp_cc_on_dupack(tcp_flow_t *f) {
    uint32_t mss = f->tx.mss ? f->tx.mss : TCP_DEFAULT_MSS;

    if (f->tx.in_fast_recovery){
        f->tx.cwnd += mss;

        tcp_tx_seg_t *s = tcp_find_sack_retransmit(f);
        if (s) {
            tcp_send_from_seg(f, s);
            s->sack_retransmitted = 1;
            s->retransmit_cnt++;
            s->timer_ms = 0;
        }

        return;
    }

    if (f->tx.dup_acks != 3) return;

    uint32_t flight = f->tx.snd_nxt - f->tx.snd_una;
    uint32_t half = flight / 2;
    uint32_t minth = 2u * mss;

    if (half < minth) half = minth;

    f->tx.ssthresh = half;
    f->tx.recover = f->tx.snd_nxt;
    f->tx.cwnd = f->tx.ssthresh + 3u * mss;
    f->tx.in_fast_recovery = 1;

    tcp_tx_seg_t *s = tcp_find_sack_retransmit(f);
    if (!s) s = tcp_find_first_unacked(f);
    if (s) {
        tcp_send_from_seg(f, s);
        s->sack_retransmitted = 1;
        s->retransmit_cnt++;
        s->timer_ms = 0;
    }
}

void tcp_input(ip_version_t ipver, const void *src_ip_addr, const void *dst_ip_addr, uint8_t l3_id, netpkt_t* pkt) {
    if (!pkt) return;
    const uint8_t* segment = (const uint8_t*)netpkt_data(pkt);
    uint32_t len = netpkt_len(pkt);
    if (len < sizeof(tcp_hdr_t)) {
        netpkt_unref(pkt);
        return;
    }

    tcp_hdr_t hdr;
    if (!netpkt_copyout(pkt, 0, &hdr, sizeof(hdr))) {
        netpkt_unref(pkt);
        return;
    }

    if (ipver == IP_VER4) {
        uint32_t src_ip = 0;
        uint32_t dst_ip = 0;
        memcpy(&src_ip, src_ip_addr, sizeof(src_ip));
        memcpy(&dst_ip, dst_ip_addr, sizeof(dst_ip));
        if (tcp_checksum_ipv4((const void*)segment, (uint16_t)len, src_ip, dst_ip) != 0) {
            netpkt_unref(pkt);
            return;
        }
    } else {
        if (tcp_checksum_ipv6((const void*)segment, (uint16_t)len, (const uint8_t *)src_ip_addr, (const uint8_t *)dst_ip_addr) != 0) {
            netpkt_unref(pkt);
            return;
        }
    }

    uint16_t src_port = bswap16(hdr.src_port);
    uint16_t dst_port = bswap16(hdr.dst_port);
    uint32_t seq = bswap32(hdr.sequence);
    uint32_t ack = bswap32(hdr.ack);
    uint8_t flags = hdr.flags;
    uint16_t window = bswap16(hdr.window);

    uint8_t hdr_len = (uint8_t)((hdr.data_offset_reserved >> 4) * 4);
    if (hdr_len < sizeof(tcp_hdr_t) || len < hdr_len) {
        netpkt_unref(pkt);
        return;
    }

    uint32_t data_len = len - hdr_len;

    tcp_parsed_opts_t parsed_opts;
    uint8_t parsed_opts_buf[40];
    uint32_t parsed_opts_len = (uint32_t)(hdr_len > sizeof(tcp_hdr_t) ? hdr_len - sizeof(tcp_hdr_t) : 0);
    if (parsed_opts_len && !netpkt_copyout(pkt, sizeof(tcp_hdr_t), parsed_opts_buf, parsed_opts_len)) {
        netpkt_unref(pkt);
        return;
    }
    tcp_parse_options(parsed_opts_len ? parsed_opts_buf : NULL, parsed_opts_len, &parsed_opts);

    int idx = -1;
    tcp_flow_t *flow = tcp_flow_acquire_match(dst_port, ipver, dst_ip_addr, src_ip_addr, src_port, &idx);
    if (flow) flow->timer.keepalive_idle_ms = 0;
    if (flow) flow->base.l3_id = l3_id;

    uint8_t ifx = 0;
    if (ipver == IP_VER4) {
        l3_ipv4_interface_t *v4 = l3_ipv4_find_by_id(l3_id);
        if (!v4 || !v4->l2) {
            if (flow) tcp_flow_put(flow);
            netpkt_unref(pkt);
            return;
        }
        ifx = v4->l2->ifindex;
    } else {
        l3_ipv6_interface_t *v6 = l3_ipv6_find_by_id(l3_id);
        if (!v6 || !v6->l2) {
            if (flow) tcp_flow_put(flow);
            netpkt_unref(pkt);
            return;
        }
        ifx = v6->l2->ifindex;
    }
    
    if (flow && flow->base.state == TCP_TIME_WAIT && (flags & (1 << SYN_F)) && !(flags& ((1 << ACK_F) | (1 << RST_F) | (1 << FIN_F))) && data_len == 0) {
        ksocket_t* listener = NULL;
        socket_bind_collect(PROTO_TCP, ipver, l3_id, ifx, dst_ip_addr, dst_port, &listener, 1);
        if (listener) {
            socket_core_put(listener);
            tcp_free_flow(idx);
            tcp_flow_put(flow);
            flow = NULL;
            idx = -1;
        }
    }

    if (flow && flow->base.state == TCP_SYN_RECEIVED && (flags & (1 << SYN_F)) && !(flags& ((1 << ACK_F) | (1 << RST_F))) && data_len == 0 && seq+1 == flow->rx.rcv_nxt) {
        for (int i = 0; i < TCP_MAX_TX_SEGS; i++) {
            tcp_tx_seg_t *seg = &flow->tx.txq[i];
            if (!seg->used || !seg->syn) continue;
            seg->timer_ms = 0;
            tcp_send_from_seg(flow, seg);
            break;
        }
        tcp_flow_put(flow);
        netpkt_unref(pkt);
        return;
    }

    if (!flow){
        ksocket_t* listener = NULL;
        socket_bind_collect(PROTO_TCP, ipver, l3_id, ifx, dst_ip_addr, dst_port, &listener, 1);

        if ((flags & (1u << SYN_F)) && !(flags & ((1u << ACK_F) | (1u << RST_F) | (1u << FIN_F))) && data_len == 0 && listener){
            rng_t rng;
            uint64_t virt_timer;
            asm volatile ("mrs %0, cntvct_el0" : "=r"(virt_timer));
            rng_seed(&rng, virt_timer);

            tcp_admit_result_t syn_admit = tcp_admit_syn(l3_id, dst_port, ipver, src_ip_addr);
            if (syn_admit != TCP_ADMIT_OK) {
                if (syn_admit == TCP_ADMIT_SYN_GLOBAL) tcp_stats.syn_drop_global++;
                else if (syn_admit == TCP_ADMIT_SYN_LISTENER) tcp_stats.syn_drop_listener++;
                else if (syn_admit == TCP_ADMIT_SYN_SOURCE) tcp_stats.syn_drop_source++;
                else if (syn_admit == TCP_ADMIT_ORPHAN_LIMIT) tcp_stats.orphan_drop_global++;
                else if (syn_admit == TCP_ADMIT_RESOURCE_BUDGET) tcp_stats.resource_budget_drop++;
                else if (syn_admit == TCP_ADMIT_FLOW_TABLE_FULL) tcp_stats.flow_table_full++;
                socket_core_put(listener);
                netpkt_unref(pkt);
                return;
            }

            const SocketExtraOptions* listener_extra = socket_tcp_extra_options(socket_core_impl(listener));
            tcp_flow_t *nf = tcp_alloc_flow();
            if (!nf) {
                socket_core_put(listener);
                netpkt_unref(pkt);
                return;
            }
            if (!tcp_flow_hold(nf)) {
                tcp_free_flow((int)nf->base.slot);
                socket_core_put(listener);
                netpkt_unref(pkt);
                return;
            }

            flow = nf;
            idx = (int)flow->base.slot;
            flow->base.local_port = dst_port;
            flow->base.l3_id = l3_id;

            flow->base.remote.ver = ipver;
            memset(flow->base.remote.ip, 0, 16);
            memcpy(flow->base.remote.ip, src_ip_addr, (uint64_t)(ipver == IP_VER6 ? 16 : 4));
            flow->base.remote.port = src_port;

            flow->base.local.ver = ipver;
            memset(flow->base.local.ip, 0, 16);
            memcpy(flow->base.local.ip, dst_ip_addr, (uint64_t)(ipver == IP_VER6 ? 16 : 4));
            flow->base.local.port = dst_port;

            flow->base.state = TCP_SYN_RECEIVED;
            flow->base.retries = TCP_SYN_RETRIES;

            tcp_parsed_opts_t pop = parsed_opts;

            flow->tx.ws_send = 0;
            flow->tx.ws_recv = 0;
            flow->tx.ws_ok = pop.has_wscale ? 1 : 0;
            if (flow->tx.ws_ok) {
                flow->tx.ws_recv = pop.wscale;
                if (flow->tx.ws_recv > 14) flow->tx.ws_recv = 14;
            }
            else {
                flow->tx.ws_send = 0;
                flow->tx.ws_recv = 0;
            }

            flow->tx.sack_ok = pop.sack_permitted ? 1 : 0;

            if (pop.has_mss && pop.mss){
                uint32_t m = pop.mss;
                uint32_t minm = ipver == IP_VER6 ? 1220u : 536u;
                uint32_t maxm = tcp_calc_mss_for_l3(l3_id, ipver, src_ip_addr);
                if (m < minm) m = minm;
                if (m > maxm) m = maxm;
                flow->tx.mss = m;
            } else flow->tx.mss = tcp_calc_mss_for_l3(l3_id, ipver, src_ip_addr);
            flow->base.ctx.flags = 0;
            flow->base.ctx.options.ptr = 0;
            flow->base.ctx.options.size = 0;
            flow->base.ctx.payload.ptr = 0;
            flow->base.ctx.payload.size = 0;

            uint32_t iss = rng_next32(&rng);

            flow->base.ctx.sequence = iss;
            flow->tx.snd_una = iss;
            flow->tx.snd_nxt = iss;

            flow->base.ctx.ack = seq + 1;
            flow->rx.rcv_nxt = seq + 1;

            flow->base.ctx.expected_ack = iss + 1;
            flow->base.ctx.ack_received = 0;
            uint32_t new_wnd = window;
            if (flow->tx.ws_ok && flow->tx.ws_recv) new_wnd <<= flow->tx.ws_recv;
            flow->tx.snd_wnd = new_wnd;

            flow->timer.persist_active = 0;
            flow->timer.persist_timer_ms = 0;
            flow->timer.persist_timeout_ms = 0;

            flow->timer.delayed_ack_pending = 0;
            flow->timer.delayed_ack_timer_ms = 0;

            uint32_t rcvbuf = TCP_DEFAULT_RCV_BUF;
            if (listener_extra && (listener_extra->flags & SOCK_OPT_BUF_SIZE) && listener_extra->buf_size) rcvbuf = listener_extra->buf_size;
            flow->rx.rcv_wnd_max = tcp_clamp_rcvbuf(rcvbuf);
            tcp_flow_apply_socket_options(&flow->base.ctx, listener_extra);
            if (flow->rx.rcv_wnd_max > 65535u && pop.has_wscale) {
                flow->tx.ws_send = 8;
                flow->tx.ws_ok = 1;
            }
            flow->rx.rcv_base = flow->rx.rcv_nxt;
            flow->rx.rcv_data_nxt = flow->rx.rcv_nxt;
            flow->rx.rcv_ooo_used = 0;
            flow->rx.sack_recent_left = 0;
            flow->rx.sack_recent_right = 0;
            flow->rx.rcv_buf = 0;
            flow->rx.rcv_adv_edge = flow->rx.rcv_nxt + flow->rx.rcv_wnd_max;
            tcp_calc_adv_wnd_field(flow, flow->tx.ws_ok ? 1 : 0);

            flow->tx.cwnd = flow->tx.mss * TCP_INIT_CWND_SEGS;
            if (flow->tx.cwnd < flow->tx.mss) flow->tx.cwnd = flow->tx.mss;
            flow->tx.ssthresh = TCP_RECV_WINDOW;
            flow->tx.dup_acks = 0;
            flow->tx.in_fast_recovery = 0;
            flow->tx.recover = 0;
            flow->tx.cwnd_acc = 0;

            flow->timer.time_wait_ms = 0;
            flow->timer.fin_wait2_ms = 0;

            tcp_tx_seg_t *seg = tcp_alloc_tx_seg(flow);
            if (!seg) {
                tcp_free_flow(idx);
                tcp_flow_put(flow);
                socket_core_put(listener);
                netpkt_unref(pkt);
                return;
            }

            seg->syn = 1;
            seg->fin = 0;
            seg->rtt_sample = 1;
            seg->retransmit_cnt = 0;
            seg->seq = iss;
            seg->len = 0;
            seg->buf = 0;
            seg->timer_ms = 0;
            seg->timeout_ms = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
            seg->opts_len = tcp_build_syn_options(seg->opts, (uint16_t)flow->tx.mss, flow->tx.ws_ok ? flow->tx.ws_send : 0xff, flow->tx.sack_ok);
            flow->tx.snd_nxt = iss + 1;
            flow->base.ctx.sequence = flow->tx.snd_nxt;
            if (!tcp_send_from_seg(flow, seg)) {
                tcp_free_flow(idx);
                tcp_flow_put(flow);
                socket_core_put(listener);
                netpkt_unref(pkt);
                return;
            }

            tcp_daemon_kick();
            socket_core_put(listener);
            tcp_flow_put(flow);
            netpkt_unref(pkt);
            return;
        }

        if (listener) socket_core_put(listener);
        if (!(flags & (1u << RST_F))){
            if (flags & (1u << ACK_F)){
                tcp_send_reset(ipver, dst_ip_addr, src_ip_addr, dst_port, src_port, ack, 0, false);
            } else {
                uint32_t seg_len = data_len;

                if (flags & (1u << SYN_F)) seg_len++;
                if (flags & (1u << FIN_F)) seg_len++;

                tcp_send_reset(ipver, dst_ip_addr, src_ip_addr, dst_port, src_port, seq, seq + seg_len, true);
            }
        }

        netpkt_unref(pkt);
        return;
    }

    if (flow->base.state == TCP_TIME_WAIT){
        if (flags & (1u << RST_F)) {
            tcp_flow_put(flow);
            netpkt_unref(pkt);
            return;
        }

        uint32_t seg_len = data_len;

        if (flags & (1u << SYN_F)) seg_len++;
        if (flags & (1u << FIN_F)) seg_len++;

        uint32_t seg_end = seq + seg_len;

        if (TCP_SEQ_LEQ(seq, flow->rx.rcv_nxt) && TCP_SEQ_GEQ(seg_end, flow->rx.rcv_nxt)){
            flow->timer.time_wait_ms = 0;
            tcp_send_ack_now(flow);
        }

        tcp_flow_put(flow);
        netpkt_unref(pkt);
        return;
    }
    uint32_t old_wnd = flow->tx.snd_wnd;
    uint32_t new_wnd = window;
    if (flow->tx.ws_ok && flow->tx.ws_recv) new_wnd <<= flow->tx.ws_recv;
    flow->tx.snd_wnd = new_wnd;

    if (flow->tx.snd_wnd > 0){
        flow->timer.persist_active = 0;
        flow->timer.persist_timer_ms = 0;
        flow->timer.persist_timeout_ms = 0;
        flow->timer.persist_probe_cnt = 0;
        if (!old_wnd) tcp_daemon_kick();
    } else {
        tcp_daemon_kick();
    }

    uint8_t fin = (flags & (1u << FIN_F)) ? 1u : 0u;

    if (flags & (1u << ACK_F)){
        if (TCP_SEQ_GT(ack, flow->tx.snd_una) && TCP_SEQ_LEQ(ack, flow->tx.snd_nxt)){
            uint32_t prev_una = flow->tx.snd_una;

            flow->tx.snd_una = ack;
            flow->base.ctx.ack_received = ack;
            flow->tx.dup_acks = 0;

            for (int i = 0; i < TCP_MAX_TX_SEGS; i++){
                tcp_tx_seg_t *s = &flow->tx.txq[i];
                if (!s->used) continue;

                uint32_t s_end = s->seq + s->len + (s->syn ? 1u : 0u) + (s->fin ? 1u : 0u);

                if (TCP_SEQ_LEQ(s_end, ack)){
                    if (s->rtt_sample && s->retransmit_cnt == 0) tcp_rtt_update(flow, s->timer_ms);

                    if (s->buf && s->len) {
                        uintptr_t seg_buf = s->buf;
                        uint32_t seg_len = (uint32_t)s->len;
                        s->buf = 0;
                        s->len = 0;
                        tcp_account_tx_remove(flow, seg_len);
                        release((void*)seg_buf);
                    }

                    s->used = 0;
                    s->buf = 0;
                    s->len = 0;
                    s->sacked = 0;
                    s->sack_retransmitted = 0;
                }
            }

            for (int i = 0; i < TCP_MAX_TX_SEGS; i++) {
                tcp_tx_seg_t *s = &flow->tx.txq[i];
                if (!s->used) continue;
                if (TCP_SEQ_GEQ(s->seq, ack)) continue;

                s->sacked = 0;
                s->sack_retransmitted = 0;
            }

            if (flow->tx.sack_ok && parsed_opts.sack_count) tcp_apply_sack_blocks(flow, &parsed_opts);
            if (TCP_SEQ_GT(ack, prev_una)) tcp_cc_on_new_ack(flow, ack);

            if (flow->base.state == TCP_FIN_WAIT_1 && TCP_SEQ_GEQ(ack, flow->base.ctx.expected_ack)){
                flow->base.state = TCP_FIN_WAIT_2;
                flow->timer.fin_wait2_ms = 0;
                tcp_daemon_kick();
            } else if ((flow->base.state == TCP_LAST_ACK || flow->base.state == TCP_CLOSING) && TCP_SEQ_GEQ(ack, flow->base.ctx.expected_ack)){
                tcp_free_flow(idx);
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }
        } else if (ack == flow->tx.snd_una && data_len == 0 && !fin){
            if (flow->tx.sack_ok && parsed_opts.sack_count) tcp_apply_sack_blocks(flow, &parsed_opts);
            if (flow->tx.dup_acks < UINT8_MAX) flow->tx.dup_acks++;
            tcp_cc_on_dupack(flow);
        } else {
            flow->tx.dup_acks = 0;
        }
    }

    if (flow->tx.nagle_len) tcp_flush_nagle(flow, flow->tx.fin_tx_pending ? 1 : 0);
    if (flow->tx.fin_tx_pending) tcp_try_send_pending_fin(flow);

    uint32_t seg_seq = seq;

    switch (flow->base.state){
    case TCP_SYN_SENT:
        if ((flags & (1u << SYN_F)) && (flags & (1u << ACK_F)) && ack == flow->base.ctx.expected_ack){
            flow->base.ctx.ack = seq + 1;
            flow->rx.rcv_nxt = seq + 1;
            flow->base.ctx.ack_received = ack;
            flow->tx.snd_una = ack;
            flow->tx.snd_nxt = flow->base.ctx.sequence;
            flow->base.ctx.sequence = flow->tx.snd_nxt;
            flow->base.ctx.flags = 0;

            tcp_parsed_opts_t pop = parsed_opts;

            flow->tx.ws_recv = pop.has_wscale ? pop.wscale : 0;
            if (flow->tx.ws_recv > 14) flow->tx.ws_recv = 14;
            flow->tx.ws_ok = (flow->tx.ws_send != 0) && pop.has_wscale ? 1 : 0;
            if (!flow->tx.ws_ok) {
                flow->tx.ws_send = 0;
                flow->tx.ws_recv = 0;
            }

            flow->tx.sack_ok = pop.sack_permitted ? 1 : 0;

            if (pop.has_mss && pop.mss){
                uint32_t m = pop.mss;
                uint32_t minm = ipver == IP_VER6 ? 1220u : 536u;
                uint32_t maxm = tcp_calc_mss_for_l3(l3_id, ipver, src_ip_addr);
                if (m < minm) m = minm;
                if (m > maxm) m = maxm;
                flow->tx.mss = m;
            } else {
                flow->tx.mss = tcp_calc_mss_for_l3(l3_id, ipver, src_ip_addr);
            }

            uint32_t new_wnd = window;
            if (flow->tx.ws_ok && flow->tx.ws_recv) new_wnd <<= flow->tx.ws_recv;
            flow->tx.snd_wnd = new_wnd;

            flow->rx.rcv_base = flow->rx.rcv_nxt;
            flow->rx.rcv_data_nxt = flow->rx.rcv_nxt;
            flow->rx.rcv_ooo_used = 0;
            flow->rx.sack_recent_left = 0;
            flow->rx.sack_recent_right = 0;
            if (!flow->rx.rcv_buf || !flow->rx.rcv_wnd_max) {
                flow->rx.rcv_wnd = 0;
                flow->rx.rcv_adv_edge = flow->rx.rcv_nxt;
                flow->base.ctx.window = 0;
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }
            (void)tcp_calc_adv_wnd_field(flow, 1);

            tcp_hdr_t final_ack;
            final_ack.src_port = bswap16(flow->base.local_port);
            final_ack.dst_port = bswap16(flow->base.remote.port);
            final_ack.sequence = bswap32(flow->base.ctx.sequence);
            final_ack.ack = bswap32(flow->base.ctx.ack);
            final_ack.flags = (uint8_t)(1u << ACK_F);
            final_ack.window = flow->base.ctx.window;
            final_ack.urgent_ptr = 0;

            if (flow->base.local.ver == IP_VER4) {
                ip_tx_opts_t tx;
                tcp_build_tx_opts_from_local_v4(flow->base.local.ip, &tx);
                tcp_send_segment(IP_VER4, flow->base.local.ip, flow->base.remote.ip, &final_ack, NULL, 0, NULL, 0, &tx, flow->ip.ttl, flow->ip.dontfrag);
            } else {
                ip_tx_opts_t tx;
                tcp_build_tx_opts_from_local_v6(flow->base.local.ip, &tx);
                tcp_send_segment(IP_VER6, flow->base.local.ip, flow->base.remote.ip, &final_ack, NULL, 0, NULL, 0, &tx, flow->ip.ttl, flow->ip.dontfrag);
            }

            flow->base.state = TCP_ESTABLISHED;
            flow->timer.delayed_ack_pending = 0;
            flow->timer.delayed_ack_timer_ms = 0;
            tcp_daemon_kick();
        } else if (flags & (1u << RST_F)){
            flow->base.state = TCP_STATE_CLOSED;
        }

        tcp_flow_put(flow);
        netpkt_unref(pkt);
        return;

    case TCP_SYN_RECEIVED:
        if ((flags & (1u << ACK_F)) && !(flags & (1u << SYN_F)) && !(flags & (1u << RST_F)) && ack == flow->base.ctx.expected_ack){
            if (!flow->rx.rcv_buf && flow->rx.rcv_wnd_max) {
                flow->rx.rcv_buf = (uintptr_t)zalloc(flow->rx.rcv_wnd_max);
                if (!flow->rx.rcv_buf) {
                    tcp_send_reset(ipver, dst_ip_addr, src_ip_addr, dst_port, src_port, 0, flow->base.ctx.ack, true);
                    tcp_free_flow(idx);
                    tcp_flow_put(flow);
                    netpkt_unref(pkt);
                    return;
                }
                flow->rx.rcv_base = flow->rx.rcv_nxt;
                flow->rx.rcv_data_nxt = flow->rx.rcv_nxt;
                tcp_calc_adv_wnd_field(flow, 1);
            }

            uint32_t queued = 0;
            ksocket_t* listener = NULL;
            socket_bind_collect(PROTO_TCP, ipver, l3_id, ifx, dst_ip_addr, dst_port, &listener, 1);
            if (listener) {
                queued = tcp_accept_enqueue(listener, ipver, src_ip_addr, dst_ip_addr, src_port, dst_port);
                socket_core_put(listener);
            }
            if (!queued) {
                tcp_stats.acceptq_drop_full++;
                tcp_hdr_t rst_hdr;
                rst_hdr.src_port = bswap16(flow->base.local_port);
                rst_hdr.dst_port = bswap16(flow->base.remote.port);
                rst_hdr.sequence = bswap32(flow->tx.snd_nxt);
                rst_hdr.ack = bswap32(flow->base.ctx.ack);
                rst_hdr.flags = (uint8_t)((1 << RST_F) | (1 << ACK_F));
                rst_hdr.window = 0;
                rst_hdr.urgent_ptr = 0;

                if (flow->base.local.ver == IP_VER4) {
                    ip_tx_opts_t tx;
                    tcp_build_tx_opts_from_local_v4(flow->base.local.ip, &tx);
                    (void)tcp_send_segment(IP_VER4, flow->base.local.ip, flow->base.remote.ip, &rst_hdr, NULL, 0, NULL, 0, &tx, flow->ip.ttl, flow->ip.dontfrag);
                } else if (flow->base.local.ver == IP_VER6) {
                    ip_tx_opts_t tx;
                    tcp_build_tx_opts_from_local_v6(flow->base.local.ip, &tx);
                    (void)tcp_send_segment(IP_VER6, flow->base.local.ip, flow->base.remote.ip, &rst_hdr, NULL, 0, NULL, 0, &tx, flow->ip.ttl, flow->ip.dontfrag);
                }

                tcp_free_flow(idx);
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }

            flow->base.ctx.sequence = flow->tx.snd_nxt;
            flow->tx.snd_una = ack;
            flow->base.state = TCP_ESTABLISHED;
            flow->timer.delayed_ack_pending = 0;
            flow->timer.delayed_ack_timer_ms = 0;
            flow->base.ctx.ack_received = ack;

            tcp_daemon_kick();
            if (!data_len && !fin) {
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }
            break;
        } else if (flags & (1u << RST_F)){
            tcp_free_flow(idx);
            tcp_flow_put(flow);
            netpkt_unref(pkt);
            return;
        }

        tcp_flow_put(flow);
        netpkt_unref(pkt);
        return;

    default:
        break;
    }

    if (flags & (1u << RST_F)) {
        tcp_free_flow(idx);
        tcp_flow_put(flow);
        netpkt_unref(pkt);
        return;
    }

    int need_ack = 0;
    int ack_immediate = 0;
    int ack_defer = 0;

    if (data_len || fin) {
        uint32_t rcv_nxt = flow->rx.rcv_nxt;
        uint32_t wnd_end = rcv_nxt + flow->rx.rcv_wnd;

        uint32_t orig_data_len = data_len;
        uint8_t fin_in = fin;
        uint8_t had_reass = flow->rx.reass_count ? 1 : 0;
        uint32_t fin_seq = seg_seq + orig_data_len;
        uint32_t orig_end = seg_seq + orig_data_len + (fin ? 1u : 0u);

        if (TCP_SEQ_LEQ(orig_end, rcv_nxt) || TCP_SEQ_GEQ(seg_seq, wnd_end)) {
            need_ack = 1;
            ack_immediate = 1;
        } else {
            if (fin_in) {
                if (TCP_SEQ_LT(fin_seq, rcv_nxt) || TCP_SEQ_GEQ(fin_seq, wnd_end)) fin_in = 0;
            }

            uint32_t payload = hdr_len;

            if (TCP_SEQ_LT(seg_seq, rcv_nxt)) {
                uint32_t d = rcv_nxt - seg_seq;
                if (d >= data_len) {
                    payload += data_len;
                    data_len = 0;
                    seg_seq = rcv_nxt;
                } else {
                    payload += d;
                    data_len -= d;
                    seg_seq = rcv_nxt;
                }
            }

            if (data_len) {
                if (TCP_SEQ_GEQ(seg_seq, wnd_end)) data_len = 0;
                else if (TCP_SEQ_GT(seg_seq + data_len, wnd_end)) data_len = wnd_end - seg_seq;
            }

            if (!data_len && !fin_in){
                need_ack = 1;
                ack_immediate = 1;
            } else if (seg_seq == flow->rx.rcv_nxt) {
                if (data_len){
                    uint32_t offer = data_len;
                    uint32_t right_edge = flow->rx.rcv_adv_edge;
                    uint32_t hard_edge = flow->rx.rcv_buf && flow->rx.rcv_wnd_max ? flow->rx.rcv_base + flow->rx.rcv_wnd_max : flow->rx.rcv_nxt;
                    if (TCP_SEQ_GT(right_edge, hard_edge)) right_edge = hard_edge;
                    if (TCP_SEQ_GEQ(seg_seq, right_edge)) offer = 0;
                    else if (TCP_SEQ_GT(seg_seq + offer, right_edge)) offer = right_edge - seg_seq;

                    uint32_t accepted = 0;
                    uint32_t stored = 0;
                    if (offer && (flow->base.state == TCP_FIN_WAIT_1 || flow->base.state == TCP_FIN_WAIT_2 || flow->base.state == TCP_CLOSING || flow->base.state == TCP_LAST_ACK || flow->base.state == TCP_TIME_WAIT)) accepted = offer;
                    else if (offer && flow->rx.rcv_buf && flow->rx.rcv_wnd_max) {
                        uint8_t *rx = (uint8_t *)flow->rx.rcv_buf;
                        uint32_t cap = flow->rx.rcv_wnd_max;
                        uint32_t pos = seg_seq % cap;
                        uint32_t first = cap - pos;

                        if (first > offer) first = offer;
                        if (first && !netpkt_copyout(pkt, payload, rx + pos, first)) stored = 0;
                        else if (offer > first && !netpkt_copyout(pkt, payload + first, rx, offer - first)) stored = 0;
                        else stored = offer;

                        accepted = stored;
                    }

                    if (accepted) {
                        flow->rx.rcv_nxt += accepted;
                        if (stored) flow->rx.rcv_data_nxt = flow->rx.rcv_nxt;
                        flow->base.ctx.ack = flow->rx.rcv_nxt;
                    } else if (data_len) {
                        (void)tcp_calc_adv_wnd_field(flow, 1);
                        need_ack = 1;
                        ack_immediate = 1;
                    }

                    if (accepted < data_len) ack_immediate = 1;
                }

                if (fin_in) {
                    if (flow->rx.rcv_nxt == fin_seq) {
                        flow->rx.rcv_nxt += 1;
                        flow->base.ctx.ack = flow->rx.rcv_nxt;

                        tcp_state_t old = flow->base.state;

                        if (old == TCP_ESTABLISHED) flow->base.state = TCP_CLOSE_WAIT;
                        else if (old == TCP_FIN_WAIT_1) flow->base.state = TCP_CLOSING;
                        else if (old == TCP_FIN_WAIT_2 || old == TCP_CLOSING || old == TCP_LAST_ACK) {
                            tcp_enter_time_wait(flow);
                        }

                        ack_immediate = 1;
                    } else {
                        flow->rx.fin_pending = 1;
                        flow->rx.fin_seq = fin_seq;
                    }
                }

                if (tcp_reass_drain_inseq(flow)) ack_immediate = 1;
                if (had_reass) ack_immediate = 1;

                if (flow->rx.fin_pending && flow->rx.fin_seq == flow->rx.rcv_nxt){
                    flow->rx.fin_pending = 0;
                    flow->rx.rcv_nxt += 1;
                    flow->base.ctx.ack = flow->rx.rcv_nxt;

                    tcp_state_t old = flow->base.state;

                    if (old == TCP_ESTABLISHED) flow->base.state = TCP_CLOSE_WAIT;
                    else if (old == TCP_FIN_WAIT_1) flow->base.state = TCP_CLOSING;
                    else if (old == TCP_FIN_WAIT_2 || old == TCP_CLOSING || old == TCP_LAST_ACK) {
                        tcp_enter_time_wait(flow);
                    }

                    ack_immediate = 1;
                }

                (void)tcp_calc_adv_wnd_field(flow, 1);

                if (!ack_immediate && data_len) ack_defer = 1;
                need_ack = 1;
            } else {
                if (!(flow->base.state == TCP_FIN_WAIT_1 || flow->base.state == TCP_FIN_WAIT_2 || flow->base.state == TCP_CLOSING || flow->base.state == TCP_LAST_ACK || flow->base.state == TCP_TIME_WAIT) && data_len && flow->rx.rcv_buf && flow->rx.rcv_wnd_max) {
                    uint32_t ooo_seq = seg_seq;
                    uint32_t ooo_data = payload;
                    uint32_t ooo_len = data_len;

                    if (TCP_SEQ_LT(ooo_seq, flow->rx.rcv_nxt)) {
                        uint32_t d = flow->rx.rcv_nxt - ooo_seq;
                        if (d >= ooo_len) ooo_len= 0;
                        else {
                            ooo_seq += d;
                            ooo_data += d;
                            ooo_len -= d;
                        }
                    }

                    if (ooo_len) {
                        uint32_t right_edge = flow->rx.rcv_adv_edge;
                        uint32_t hard_edge = flow->rx.rcv_base + flow->rx.rcv_wnd_max;
                        if (TCP_SEQ_GT(right_edge, hard_edge)) right_edge = hard_edge;
                        if (TCP_SEQ_GEQ(ooo_seq, right_edge)) ooo_len = 0;
                        else if (TCP_SEQ_GT(ooo_seq + ooo_len, right_edge)) ooo_len = right_edge - ooo_seq;
                    }

                    if (ooo_len) {
                        uint32_t start = ooo_seq;
                        uint32_t end = ooo_seq + ooo_len;
                        uint32_t merged_start = start;
                        uint32_t merged_end = end;
                        uint32_t old_bytes = 0;
                        int16_t overlapping = 0;
                        int16_t covered = 0;

                        if (!tcp_reass_count_ok(flow)) ooo_len = 0;
                        for (int i = 0; ooo_len && i < flow->rx.reass_count; i++) {
                            tcp_reass_seg_t *r = &flow->rx.reass[i];
                            if (TCP_SEQ_LEQ(r->seq, start) && TCP_SEQ_GEQ(r->end, end)) {
                                flow->rx.sack_recent_left = r->seq;
                                flow->rx.sack_recent_right = r->end;
                                covered = 1;
                                break;
                            }

                            if (TCP_SEQ_LT(r->end, merged_start) || TCP_SEQ_GT(r->seq, merged_end)) continue;

                            if (TCP_SEQ_LT(r->seq, merged_start)) merged_start = r->seq;
                            if (TCP_SEQ_GT(r->end, merged_end)) merged_end = r->end;
                            if (TCP_SEQ_GT(r->end, r->seq)) old_bytes += r->end - r->seq;
                            overlapping++;
                        }

                        if (!covered && ooo_len) {
                            uint32_t merged_len = merged_end - merged_start;
                            uint32_t increase = merged_len > old_bytes ? merged_len - old_bytes : 0;
                            int remaining = flow->rx.reass_count - overlapping;

                            if (remaining < 0) remaining = 0;
                            tcp_admit_result_t ooo_admit = tcp_admit_ooo(flow, increase, (uint32_t)remaining);
                            if (ooo_admit != TCP_ADMIT_OK) {
                                if (ooo_admit == TCP_ADMIT_OOO_FLOW_BYTES) tcp_stats.ooo_drop_flow_bytes++;
                                else if (ooo_admit == TCP_ADMIT_OOO_FLOW_SEGS) tcp_stats.ooo_drop_flow_segs++;
                                else if (ooo_admit == TCP_ADMIT_OOO_GLOBAL_BYTES) tcp_stats.ooo_drop_global_bytes++;
                                else if (ooo_admit == TCP_ADMIT_OOO_GLOBAL_SEGS) tcp_stats.ooo_drop_global_segs++;
                                ooo_len = 0;
                            }
                        }

                        if (!covered && ooo_len) {
                            uint8_t *rx = (uint8_t *)flow->rx.rcv_buf;
                            uint32_t cap = flow->rx.rcv_wnd_max;
                            uint32_t pos = start % cap;
                            uint32_t first = cap - pos;
                            int copied = 1;

                            if (first > ooo_len) first = ooo_len;
                            if (first && !netpkt_copyout(pkt, ooo_data, rx + pos, first)) copied = 0;
                            if (copied && ooo_len > first && !netpkt_copyout(pkt, ooo_data + first, rx, ooo_len - first)) copied = 0;

                            if (copied) {
                                for (int i = 0; i < flow->rx.reass_count;) {
                                    tcp_reass_seg_t *r = &flow->rx.reass[i];
                                    if (TCP_SEQ_LT(r->end, merged_start) || TCP_SEQ_GT(r->seq, merged_end)) {
                                        i++;
                                        continue;
                                    }
                
                                    tcp_reass_remove(flow,i);
                                }

                                if (flow->rx.reass_count < TCP_REASS_MAX_SEGS) {
                                    int pos_idx = flow->rx.reass_count;
                                    while (pos_idx > 0 && TCP_SEQ_GT(flow->rx.reass[pos_idx - 1].seq, merged_start)) {
                                        flow->rx.reass[pos_idx] = flow->rx.reass[pos_idx - 1];
                                        pos_idx--;
                                    }

                                    flow->rx.reass[pos_idx].seq = merged_start;
                                    flow->rx.reass[pos_idx].end = merged_end;
                                    flow->rx.reass_count++;
                                    flow->rx.rcv_ooo_used += merged_end - merged_start;
                                    tcp_account_ooo_add(merged_end - merged_start, 1);
                                    flow->rx.sack_recent_left = merged_start;
                                    flow->rx.sack_recent_right = merged_end;
                                    tcp_calc_adv_wnd_field(flow, 1);
                                }
                            }
                        }
                    }
                }

                if (fin_in){
                    flow->rx.fin_pending = 1;
                    flow->rx.fin_seq = fin_seq;
                }

                need_ack = 1;
                ack_immediate = 1;
            }
        }
    }

    if (need_ack){
        if (ack_immediate){
            tcp_send_ack_now(flow);
        } else if (ack_defer){
            if (!flow->timer.delayed_ack_pending){
                flow->timer.delayed_ack_pending = 1;
                flow->timer.delayed_ack_timer_ms = 0;
                tcp_daemon_kick();
            } else {
                tcp_send_ack_now(flow);
            }
        } else {
            if (!flow->timer.delayed_ack_pending){
                flow->timer.delayed_ack_pending = 1;
                flow->timer.delayed_ack_timer_ms = 0;
                tcp_daemon_kick();
            } else {
                tcp_send_ack_now(flow);
            }
        }
    }
    tcp_flow_put(flow);
    netpkt_unref(pkt);
}

void tcp_flow_on_app_read(tcp_data *flow_ctx, uint32_t bytes_read){
    if (!flow_ctx || bytes_read == 0) return;

    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow) return;

    uint32_t old_edge = flow->rx.rcv_adv_edge;
    uint16_t old_adv_field = flow->base.ctx.window;
    uint32_t old_nxt = flow->rx.rcv_nxt;

    int advanced = tcp_reass_drain_inseq(flow);
    tcp_calc_adv_wnd_field(flow, 1);

    if (flow->base.state != TCP_STATE_CLOSED && flow->base.state != TCP_TIME_WAIT) {
        uint32_t threshold = flow->tx.mss ? flow->tx.mss : TCP_DEFAULT_MSS;
        uint32_t half = flow->rx.rcv_wnd_max >> 1;
        if (half && half < threshold) threshold = half;
        if (!threshold) threshold = 1;

        uint32_t delta = TCP_SEQ_GT(flow->rx.rcv_adv_edge, old_edge) ? flow->rx.rcv_adv_edge - old_edge : 0;
        if (advanced || flow->rx.rcv_nxt != old_nxt || (!old_adv_field && flow->base.ctx.window) || delta >= threshold) tcp_send_ack_now(flow);
    }

    tcp_flow_put(flow);
}
