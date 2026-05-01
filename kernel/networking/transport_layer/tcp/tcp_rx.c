#include "tcp_internal.h"
#include "networking/port_manager.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "std/memory.h"
#include "math/rng.h"
#include "syscalls/syscalls.h"
#include "../tcp.h"

static tcp_flow_t *tcp_flow_from_ctx(tcp_data *flow_ctx) {
    if (!flow_ctx) return NULL;
    for (int i = 0; i < MAX_TCP_FLOWS; i++) {
        tcp_flow_t *flow = tcp_flows[i];
        if (!flow)continue;
        if (&flow->ctx == flow_ctx) return flow;
    }

    return NULL;
}

static void tcp_reass_remove(tcp_flow_t *flow, int idx) {
    uint32_t len = flow->reass[idx].end - flow->reass[idx].seq;

    if (flow->rcv_ooo_used >= len) flow->rcv_ooo_used -= len;
    else flow->rcv_ooo_used = 0;

    for (int i = idx; i + 1 < flow->reass_count; i++) flow->reass[i] = flow->reass[i+1];

    if (flow->reass_count) flow->reass_count--;
    flow->reass[flow->reass_count].seq = 0;
    flow->reass[flow->reass_count].end = 0;
}

static int tcp_reass_drain_inseq(tcp_flow_t *flow) {
    int advanced = 0;

    for(;;){
        int idx = -1;

        for (int i = 0; i < flow->reass_count; i++){
            if (flow->reass[i].seq != flow->rcv_nxt) continue;
            idx = i;
            break;
        }

        if (idx < 0) break;

        uint32_t end = flow->reass[idx].end;
        if (end <= flow->rcv_nxt) {
            tcp_reass_remove(flow, idx);
            continue;
        }

        flow->rcv_nxt = end;
        flow->rcv_data_nxt = end;
        flow->ctx.ack = flow->rcv_nxt;
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

    if (!flow->rcv_buf || !flow->rcv_wnd_max) return 0;
    if (flow->rcv_data_nxt < flow->rcv_base) return 0;

    uint32_t n = flow->rcv_data_nxt - flow->rcv_base;
    if (!n) return 0;
    if (len < n) n = (uint32_t)len;

    uint8_t *rx = (uint8_t*)flow->rcv_buf;
    uint8_t *dst = (uint8_t*)buf;
    uint32_t pos = flow->rcv_base % flow->rcv_wnd_max;
    uint32_t first = flow->rcv_wnd_max - pos;

    if (first > n) first = n;
    if (first) memcpy(dst, rx + pos, first);
    if (n > first) memcpy(dst + first, rx, n - first);

    flow->rcv_base += n;
    tcp_flow_on_app_read(flow_ctx, n);
    return n;
}

uint32_t tcp_flow_readable(tcp_data *flow_ctx) {
    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow || !flow->rcv_buf || !flow->rcv_wnd_max) return 0;
    if (flow->rcv_data_nxt < flow->rcv_base) return 0;
    return flow->rcv_data_nxt - flow->rcv_base;
}

bool tcp_flow_recv_closed(tcp_data *flow_ctx) {
    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow) return true;
    if (flow->state == TCP_CLOSE_WAIT || flow->state == TCP_STATE_CLOSED || flow->state == TCP_TIME_WAIT) return true;
    return false;
}

tcp_tx_seg_t *tcp_find_first_unacked(tcp_flow_t *flow) {
    tcp_tx_seg_t *best = NULL;
    uint32_t best_seq = 0;

    for (int i = 0; i < TCP_MAX_TX_SEGS; i++){
        tcp_tx_seg_t *s = &flow->txq[i];

        if (!s->used) continue;

        uint32_t end = s->seq + s->len + (s->syn ? 1u : 0u) + (s->fin ? 1u : 0u);
        if (end <= flow->snd_una) continue;

        if (!best || s->seq < best_seq){
            best = s;
            best_seq = s->seq;
        }
    }

    return best;
}

void tcp_cc_on_timeout(tcp_flow_t *f){
    uint32_t mss = f->mss ? f->mss : TCP_DEFAULT_MSS;
    uint32_t flight = f->snd_nxt > f->snd_una ? f->snd_nxt - f->snd_una : 0;
    uint32_t half = flight / 2;
    uint32_t minth = 2u * mss;

    if (half < minth) half = minth;

    f->ssthresh = half;
    f->cwnd = mss;
    f->cwnd_acc = 0;
    f->dup_acks = 0;
    f->in_fast_recovery = 0;
    f->recover = 0;
}

static void tcp_cc_on_new_ack(tcp_flow_t *f, uint32_t ack) {
    uint32_t mss = f->mss ? f->mss : TCP_DEFAULT_MSS;

    if (f->in_fast_recovery){
        if (ack >= f->recover){
            f->cwnd = f->ssthresh;
            if (f->cwnd < mss) f->cwnd = mss;

            f->in_fast_recovery = 0;
            f->dup_acks = 0;
            f->cwnd_acc = 0;
            return;
        }

        f->cwnd = f->ssthresh;
        if (f->cwnd < mss) f->cwnd = mss;
        return;
    }

    if (f->cwnd < f->ssthresh){
        f->cwnd += mss;
        if (f->cwnd < mss) f->cwnd = mss;
        return;
    }

    uint32_t denom = f->cwnd ? f->cwnd : 1u;
    uint32_t inc = (mss * mss) / denom;

    if (inc == 0) inc = 1;

    f->cwnd += inc;
}

static void tcp_cc_on_dupack(tcp_flow_t *f) {
    uint32_t mss = f->mss ? f->mss : TCP_DEFAULT_MSS;

    if (f->in_fast_recovery){
        f->cwnd += mss;
        return;
    }

    if (f->dup_acks != 3) return;

    uint32_t flight = f->snd_nxt - f->snd_una;
    uint32_t half = flight / 2;
    uint32_t minth = 2u * mss;

    if (half < minth) half = minth;

    f->ssthresh = half;
    f->recover = f->snd_nxt;
    f->cwnd = f->ssthresh + 3u * mss;
    f->in_fast_recovery = 1;

    tcp_tx_seg_t *s = tcp_find_first_unacked(f);
    if (s) {
        tcp_send_from_seg(f, s);
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
    if (len < hdr_len) {
        netpkt_unref(pkt);
        return;
    }

    uint32_t data_len = len - hdr_len;

    int idx = find_flow(dst_port, ipver, dst_ip_addr, src_ip_addr, src_port);
    tcp_flow_t *flow = idx >= 0 ? tcp_flows[idx] : NULL;
    if (flow) flow->keepalive_idle_ms = 0;
    if (flow) flow->l3_id = l3_id;

    port_manager_t *pm = NULL;
    uint8_t ifx = 0;

    if (ipver == IP_VER4) {
        l3_ipv4_interface_t *v4 = l3_ipv4_find_by_id(l3_id);
        if (!v4 || !v4->l2) {
            netpkt_unref(pkt);
            return;
        }
        pm = ifmgr_pm_v4(l3_id);
        ifx = v4->l2->ifindex;
    } else {
        l3_ipv6_interface_t *v6 = l3_ipv6_find_by_id(l3_id);
        if (!v6 || !v6->l2) {
            netpkt_unref(pkt);
            return;
        }
        pm = ifmgr_pm_v6(l3_id);
        ifx = v6->l2->ifindex;
    }
    
    if (!pm) {
        netpkt_unref(pkt);
        return;
    }

    port_recv_handler_t port_handler = port_get_handler(pm, PROTO_TCP, dst_port);

    if (!flow){
        int listen_idx = find_flow(dst_port, ipver, dst_ip_addr, NULL, 0);
        if (listen_idx < 0)
            listen_idx = find_flow(dst_port, ipver, NULL, NULL, 0);

        if ((flags & (1u << SYN_F)) && !(flags & (1u << ACK_F)) && listen_idx >= 0){
            rng_t rng;
            uint64_t virt_timer;
            asm volatile ("mrs %0, cntvct_el0" : "=r"(virt_timer));
            rng_seed(&rng, virt_timer);

            int syn_total = 0;
            int syn_port = 0;
            for (int k = 0; k < MAX_TCP_FLOWS; k++){
                tcp_flow_t *f = tcp_flows[k];
                if (!f) continue;
                if (f->state != TCP_SYN_RECEIVED) continue;
                syn_total++;
                if (f->local_port == dst_port && f->l3_id == l3_id) syn_port++;
            }
            if (syn_total >= (MAX_TCP_FLOWS / 4) || syn_port >= 32) {
                netpkt_unref(pkt);
                return;
            }

            tcp_flow_t *lf = tcp_flows[listen_idx];
            tcp_flow_t *nf = tcp_alloc_flow();
            if (!nf) {
                netpkt_unref(pkt);
                return;
            }

            flow = nf;
            for (int k = 0; k < MAX_TCP_FLOWS; k++) {
                if (tcp_flows[k] == nf) {
                    idx = k;
                    break;
                }
            }

            flow->local_port = dst_port;
            flow->l3_id = l3_id;

            flow->remote.ver = ipver;
            memset(flow->remote.ip, 0, 16);
            memcpy(flow->remote.ip, src_ip_addr, (uint64_t)(ipver == IP_VER6 ? 16 : 4));
            flow->remote.port = src_port;

            flow->local.ver = ipver;
            memset(flow->local.ip, 0, 16);
            memcpy(flow->local.ip, dst_ip_addr, (uint64_t)(ipver == IP_VER6 ? 16 : 4));
            flow->local.port = dst_port;

            flow->state = TCP_SYN_RECEIVED;
            flow->retries = TCP_SYN_RETRIES;

            tcp_parsed_opts_t pop;
            uint8_t rx_opts_buf[40];
            uint32_t rx_opts_len = (uint32_t)(hdr_len > sizeof(tcp_hdr_t) ? hdr_len - sizeof(tcp_hdr_t) : 0);
            if (rx_opts_len && (!netpkt_copyout(pkt, sizeof(tcp_hdr_t), rx_opts_buf, rx_opts_len))) {
                tcp_free_flow(idx);
                netpkt_unref(pkt);
                return;
            }
            tcp_parse_options(rx_opts_len ? rx_opts_buf : NULL, rx_opts_len, &pop);

            flow->ws_send = lf->ws_send;
            flow->ws_recv = 0;
            flow->ws_ok = (lf->ws_ok && pop.has_wscale) ? 1 : 0;
            if (flow->ws_ok) {
                flow->ws_recv = pop.wscale;
                if (flow->ws_recv > 14) flow->ws_recv = 14;
            }
            else {
                flow->ws_send = 0;
                flow->ws_recv = 0;
            }

            flow->sack_ok = (lf->sack_ok && pop.sack_permitted) ? 1 : 0;

            if (pop.has_mss && pop.mss){
                uint32_t m = pop.mss;
                uint32_t minm = ipver == IP_VER6 ? 1220u : 536u;
                uint32_t maxm = tcp_calc_mss_for_l3(l3_id, ipver, src_ip_addr);
                if (m < minm) m = minm;
                if (m > maxm) m = maxm;
                flow->mss = m;
            } else flow->mss = tcp_calc_mss_for_l3(l3_id, ipver, src_ip_addr);
            flow->ctx.flags = 0;
            flow->ctx.options = lf->ctx.options;
            flow->ctx.payload.ptr = 0;
            flow->ctx.payload.size = 0;

            uint32_t iss = rng_next32(&rng);

            flow->ctx.sequence = iss;
            flow->snd_una = iss;
            flow->snd_nxt = iss;

            flow->ctx.ack = seq + 1;
            flow->rcv_nxt = seq + 1;

            flow->ctx.expected_ack = iss + 1;
            flow->ctx.ack_received = 0;
            uint32_t new_wnd = window;
            if (flow->ws_ok && flow->ws_recv) new_wnd <<= flow->ws_recv;
            flow->snd_wnd = new_wnd;

            flow->persist_active = 0;
            flow->persist_timer_ms = 0;
            flow->persist_timeout_ms = 0;

            flow->delayed_ack_pending = 0;
            flow->delayed_ack_timer_ms = 0;

            flow->rcv_wnd_max = lf->rcv_wnd_max;
            flow->rcv_base = flow->rcv_nxt;
            flow->rcv_data_nxt = flow->rcv_nxt;
            flow->rcv_ooo_used = 0;
            flow->rcv_buf = (uintptr_t)zalloc(flow->rcv_wnd_max);
            if (!flow->rcv_buf) {
                flow->rcv_wnd = 0;
                flow->rcv_adv_edge = flow->rcv_nxt;
                flow->ctx.window = 0;
                tcp_free_flow(idx);
                netpkt_unref(pkt);
                return;
            }
            tcp_calc_adv_wnd_field(flow, flow->ws_ok ? 1 : 0);

            flow->ip_ttl = lf->ip_ttl;
            flow->ip_dontfrag = lf->ip_dontfrag;
            flow->keepalive_on = lf->keepalive_on;
            flow->keepalive_ms = lf->keepalive_ms;
            flow->keepalive_idle_ms = 0;

            flow->cwnd = flow->mss;
            flow->ssthresh = TCP_RECV_WINDOW;
            flow->dup_acks = 0;
            flow->in_fast_recovery = 0;
            flow->recover = 0;
            flow->cwnd_acc = 0;

            flow->time_wait_ms = 0;
            flow->fin_wait2_ms = 0;

            tcp_tx_seg_t *seg = tcp_alloc_tx_seg(flow);
            if (!seg) {
                tcp_free_flow(idx);
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
            seg->timeout_ms = flow->rto ? flow->rto : TCP_INIT_RTO;
            seg->opts_len = tcp_build_syn_options(seg->opts, (uint16_t)flow->mss, flow->ws_ok ? flow->ws_send : 0xff, flow->sack_ok);
            tcp_send_from_seg(flow, seg);

            flow->snd_nxt = iss + 1;

            tcp_daemon_kick();
            netpkt_unref(pkt);
            return;
        }

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

    if (flow->state == TCP_TIME_WAIT){
        if (flags & (1u << RST_F)) {
            netpkt_unref(pkt);
            return;
        }

        uint32_t seg_len = data_len;

        if (flags & (1u << SYN_F)) seg_len++;
        if (flags & (1u << FIN_F)) seg_len++;

        uint32_t seg_end = seq + seg_len;

        if (seq <= flow->rcv_nxt && seg_end >= flow->rcv_nxt){
            flow->time_wait_ms = 0;
            tcp_send_ack_now(flow);
        }

        netpkt_unref(pkt);
        return;
    }
    uint32_t new_wnd = window;
    if (flow->ws_ok && flow->ws_recv) new_wnd <<= flow->ws_recv;
    flow->snd_wnd = new_wnd;

    if (flow->snd_wnd > 0){
        flow->persist_active = 0;
        flow->persist_timer_ms = 0;
        flow->persist_timeout_ms = 0;
        flow->persist_probe_cnt = 0;
    } else {
        tcp_daemon_kick();
    }

    uint8_t fin = (flags & (1u << FIN_F)) ? 1u : 0u;

    if (flags & (1u << ACK_F)){
        if (ack > flow->snd_una && ack <= flow->snd_nxt){
            uint32_t prev_una = flow->snd_una;

            flow->snd_una = ack;
            flow->ctx.ack_received = ack;
            flow->dup_acks = 0;

            for (int i = 0; i < TCP_MAX_TX_SEGS; i++){
                tcp_tx_seg_t *s = &flow->txq[i];
                if (!s->used) continue;

                uint32_t s_end = s->seq + s->len + (s->syn ? 1u : 0u) + (s->fin ? 1u : 0u);

                if (s_end <= ack){
                    if (s->rtt_sample && s->retransmit_cnt == 0) tcp_rtt_update(flow, s->timer_ms);

                    if (s->buf && s->len) release((void *)s->buf);

                    s->used = 0;
                    s->buf = 0;
                    s->len = 0;
                }
            }

            if (ack > prev_una) tcp_cc_on_new_ack(flow, ack);

            if (flow->state == TCP_FIN_WAIT_1 && ack >= flow->ctx.expected_ack){
                flow->state = TCP_FIN_WAIT_2;
                flow->fin_wait2_ms = 0;
                tcp_daemon_kick();
            } else if ((flow->state == TCP_LAST_ACK || flow->state == TCP_CLOSING) && ack >= flow->ctx.expected_ack){
                tcp_free_flow(idx);
                netpkt_unref(pkt);
                return;
            }
        } else if (ack == flow->snd_una && data_len == 0 && !fin){
            if (flow->dup_acks < UINT8_MAX) flow->dup_acks++;
            tcp_cc_on_dupack(flow);
        } else {
            flow->dup_acks = 0;
        }
    }

    uint32_t seg_seq = seq;

    switch (flow->state){
    case TCP_SYN_SENT:
        if ((flags & (1u << SYN_F)) && (flags & (1u << ACK_F)) && ack == flow->ctx.expected_ack){
            flow->ctx.ack = seq + 1;
            flow->rcv_nxt = seq + 1;
            flow->ctx.ack_received = ack;
            flow->snd_una = ack;
            flow->snd_nxt = flow->ctx.sequence;
            flow->ctx.sequence = flow->snd_nxt;
            flow->ctx.flags = 0;

            tcp_parsed_opts_t pop;
            uint8_t ack_opts_buf[40];
            uint32_t ack_opts_len = (uint32_t)(hdr_len > sizeof(tcp_hdr_t) ? hdr_len - sizeof(tcp_hdr_t) : 0);
            if (ack_opts_len && (!netpkt_copyout(pkt, sizeof(tcp_hdr_t), ack_opts_buf, ack_opts_len))) {
                netpkt_unref(pkt);
                return;
            }
            tcp_parse_options(ack_opts_len ? ack_opts_buf : NULL, ack_opts_len, &pop);

            flow->ws_recv = pop.has_wscale ? pop.wscale : 0;
            if (flow->ws_recv > 14) flow->ws_recv = 14;
            flow->ws_ok = (flow->ws_send != 0) && pop.has_wscale ? 1 : 0;
            if (!flow->ws_ok) {
                flow->ws_send = 0;
                flow->ws_recv = 0;
            }

            flow->sack_ok = pop.sack_permitted ? 1 : 0;

            if (pop.has_mss && pop.mss){
                uint32_t m = pop.mss;
                uint32_t minm = ipver == IP_VER6 ? 1220u : 536u;
                uint32_t maxm = tcp_calc_mss_for_l3(l3_id, ipver, src_ip_addr);
                if (m < minm) m = minm;
                if (m > maxm) m = maxm;
                flow->mss = m;
            } else {
                flow->mss = tcp_calc_mss_for_l3(l3_id, ipver, src_ip_addr);
            }

            uint32_t new_wnd = window;
            if (flow->ws_ok && flow->ws_recv) new_wnd <<= flow->ws_recv;
            flow->snd_wnd = new_wnd;

            flow->rcv_base = flow->rcv_nxt;
            flow->rcv_data_nxt = flow->rcv_nxt;
            flow->rcv_ooo_used = 0;
            if (!flow->rcv_buf || !flow->rcv_wnd_max) {
                flow->rcv_wnd = 0;
                flow->rcv_adv_edge = flow->rcv_nxt;
                flow->ctx.window = 0;
                netpkt_unref(pkt);
                return;
            }
            (void)tcp_calc_adv_wnd_field(flow, 1);

            tcp_hdr_t final_ack;
            final_ack.src_port = bswap16(flow->local_port);
            final_ack.dst_port = bswap16(flow->remote.port);
            final_ack.sequence = bswap32(flow->ctx.sequence);
            final_ack.ack = bswap32(flow->ctx.ack);
            final_ack.flags = (uint8_t)(1u << ACK_F);
            final_ack.window = flow->ctx.window;
            final_ack.urgent_ptr = 0;

            if (flow->local.ver == IP_VER4) {
                ipv4_tx_opts_t tx;
                tcp_build_tx_opts_from_local_v4(flow->local.ip, &tx);
                tcp_send_segment(IP_VER4, flow->local.ip, flow->remote.ip, &final_ack, NULL, 0, NULL, 0, (const ip_tx_opts_t *)&tx, flow->ip_ttl, flow->ip_dontfrag);
            } else {
                ipv6_tx_opts_t tx;
                tcp_build_tx_opts_from_local_v6(flow->local.ip, &tx);
                tcp_send_segment(IP_VER6, flow->local.ip, flow->remote.ip, &final_ack, NULL, 0, NULL, 0, (const ip_tx_opts_t *)&tx, flow->ip_ttl, flow->ip_dontfrag);
            }

            flow->state = TCP_ESTABLISHED;
            flow->delayed_ack_pending = 0;
            flow->delayed_ack_timer_ms = 0;
            tcp_daemon_kick();
        } else if (flags & (1u << RST_F)){
            flow->state = TCP_STATE_CLOSED;
        }

        netpkt_unref(pkt);
        return;

    case TCP_SYN_RECEIVED:
        if ((flags & (1u << ACK_F)) && !(flags & (1u << SYN_F)) && !(flags & (1u << RST_F)) && ack == flow->ctx.expected_ack){
            uint32_t queued = 0;
            if (port_handler) queued = port_handler(ifx, ipver, src_ip_addr, dst_ip_addr, 0, src_port, dst_port);
            if (!queued) {
                tcp_hdr_t rst_hdr;
                rst_hdr.src_port = bswap16(flow->local_port);
                rst_hdr.dst_port = bswap16(flow->remote.port);
                rst_hdr.sequence = bswap32(flow->snd_nxt);
                rst_hdr.ack = bswap32(flow->ctx.ack);
                rst_hdr.flags = (uint8_t)((1 << RST_F) | (1 << ACK_F));
                rst_hdr.window = 0;
                rst_hdr.urgent_ptr = 0;

                if (flow->local.ver == IP_VER4) {
                    ipv4_tx_opts_t tx;
                    tcp_build_tx_opts_from_local_v4(flow->local.ip, &tx);
                    (void)tcp_send_segment(IP_VER4, flow->local.ip, flow->remote.ip, &rst_hdr, NULL, 0, NULL, 0, (const ip_tx_opts_t *)&tx, flow->ip_ttl, flow->ip_dontfrag);
                } else if (flow->local.ver == IP_VER6) {
                    ipv6_tx_opts_t tx;
                    tcp_build_tx_opts_from_local_v6(flow->local.ip, &tx);
                    (void)tcp_send_segment(IP_VER6, flow->local.ip, flow->remote.ip, &rst_hdr, NULL, 0, NULL, 0, (const ip_tx_opts_t *)&tx, flow->ip_ttl, flow->ip_dontfrag);
                }

                tcp_free_flow(idx);
                netpkt_unref(pkt);
                return;
            }

            flow->ctx.sequence = flow->snd_nxt;
            flow->snd_una = ack;
            flow->state = TCP_ESTABLISHED;
            flow->delayed_ack_pending = 0;
            flow->delayed_ack_timer_ms = 0;
            flow->ctx.ack_received = ack;

            tcp_daemon_kick();
        } else if (flags & (1u << RST_F)){
            tcp_free_flow(idx);
        }

        netpkt_unref(pkt);
        return;

    default:
        break;
    }

    if (flags & (1u << RST_F)) {
        tcp_free_flow(idx);
        netpkt_unref(pkt);
        return;
    }

    int need_ack = 0;
    int ack_immediate = 0;
    int ack_defer = 0;

    if (data_len || fin) {
        uint32_t rcv_nxt = flow->rcv_nxt;
        uint32_t wnd_end = rcv_nxt + flow->rcv_wnd;

        uint32_t orig_data_len = data_len;
        uint8_t fin_in = fin;
        uint32_t fin_seq = seg_seq + orig_data_len;
        uint32_t orig_end = seg_seq + orig_data_len + (fin ? 1u : 0u);

        if (orig_end <= rcv_nxt || seg_seq >= wnd_end) {
            need_ack = 1;
            ack_immediate = 1;
        } else {
            if (fin_in) {
                if (fin_seq < rcv_nxt || fin_seq >= wnd_end) fin_in = 0;
            }

            uint32_t payload = hdr_len;

            if (seg_seq < rcv_nxt) {
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
                if (seg_seq >= wnd_end) data_len = 0;
                else if (seg_seq + data_len > wnd_end) data_len = wnd_end - seg_seq;
            }

            if (!data_len && !fin_in){
                need_ack = 1;
                ack_immediate = 1;
            } else if (seg_seq == flow->rcv_nxt) {
                if (data_len){
                    uint32_t offer = data_len;
                    uint32_t right_edge = flow->rcv_adv_edge;
                    uint32_t hard_edge = flow->rcv_buf && flow->rcv_wnd_max ? flow->rcv_base + flow->rcv_wnd_max : flow->rcv_nxt;
                    if (right_edge > hard_edge) right_edge = hard_edge;
                    if (seg_seq >= right_edge) offer = 0;
                    else if (seg_seq + offer > right_edge) offer = right_edge - seg_seq;

                    uint32_t accepted = 0;
                    uint32_t stored = 0;
                    if (offer && (flow->state == TCP_FIN_WAIT_1 || flow->state == TCP_FIN_WAIT_2 || flow->state == TCP_CLOSING || flow->state == TCP_LAST_ACK || flow->state == TCP_TIME_WAIT)) accepted = offer;
                    else if (offer && flow->rcv_buf && flow->rcv_wnd_max) {
                        uint8_t *rx = (uint8_t *)flow->rcv_buf;
                        uint32_t cap = flow->rcv_wnd_max;
                        uint32_t pos = seg_seq % cap;
                        uint32_t first = cap - pos;

                        if (first > offer) first = offer;
                        if (first && !netpkt_copyout(pkt, payload, rx + pos, first)) stored = 0;
                        else if (offer > first && !netpkt_copyout(pkt, payload + first, rx, offer - first)) stored = 0;
                        else stored = offer;

                        accepted = stored;
                    }

                    if (accepted) {
                        flow->rcv_nxt += accepted;
                        if (stored) flow->rcv_data_nxt = flow->rcv_nxt;
                        flow->ctx.ack = flow->rcv_nxt;
                    } else if (data_len) {
                        (void)tcp_calc_adv_wnd_field(flow, 1);
                        need_ack = 1;
                        ack_immediate = 1;
                    }

                    if (accepted < data_len) ack_immediate = 1;
                }

                if (fin_in) {
                    if (flow->rcv_nxt == fin_seq) {
                        flow->rcv_nxt += 1;
                        flow->ctx.ack = flow->rcv_nxt;

                        tcp_state_t old = flow->state;

                        if (old == TCP_ESTABLISHED) flow->state = TCP_CLOSE_WAIT;
                        else if (old == TCP_FIN_WAIT_1) flow->state = TCP_CLOSING;
                        else if (old == TCP_FIN_WAIT_2 || old == TCP_CLOSING || old == TCP_LAST_ACK) {
                            flow->state = TCP_TIME_WAIT;
                            flow->time_wait_ms = 0;
                            tcp_daemon_kick();
                        }

                        ack_immediate = 1;
                    } else {
                        flow->fin_pending = 1;
                        flow->fin_seq = fin_seq;
                    }
                }

                if (tcp_reass_drain_inseq(flow)) ack_immediate = 1;

                if (flow->fin_pending && flow->fin_seq == flow->rcv_nxt){
                    flow->fin_pending = 0;
                    flow->rcv_nxt += 1;
                    flow->ctx.ack = flow->rcv_nxt;

                    tcp_state_t old = flow->state;

                    if (old == TCP_ESTABLISHED) flow->state = TCP_CLOSE_WAIT;
                    else if (old == TCP_FIN_WAIT_1) flow->state = TCP_CLOSING;
                    else if (old == TCP_FIN_WAIT_2 || old == TCP_CLOSING || old == TCP_LAST_ACK) {
                        flow->state = TCP_TIME_WAIT;
                        flow->time_wait_ms = 0;
                        tcp_daemon_kick();
                    }

                    ack_immediate = 1;
                }

                (void)tcp_calc_adv_wnd_field(flow, 1);

                if (!ack_immediate && data_len) ack_defer = 1;
                need_ack = 1;
            } else {
                if (!(flow->state == TCP_FIN_WAIT_1 || flow->state == TCP_FIN_WAIT_2 || flow->state == TCP_CLOSING || flow->state == TCP_LAST_ACK || flow->state == TCP_TIME_WAIT) && data_len && flow->rcv_buf && flow->rcv_wnd_max) {
                    uint32_t ooo_seq = seg_seq;
                    uint32_t ooo_data = payload;
                    uint32_t ooo_len = data_len;

                    if (ooo_seq <flow->rcv_nxt) {
                        uint32_t d = flow->rcv_nxt - ooo_seq;
                        if (d >= ooo_len) ooo_len= 0;
                        else {
                            ooo_seq += d;
                            ooo_data += d;
                            ooo_len -= d;
                        }
                    }

                    if (ooo_len) {
                        uint32_t right_edge = flow->rcv_adv_edge;
                        uint32_t hard_edge = flow->rcv_base + flow->rcv_wnd_max;
                        if (right_edge > hard_edge) right_edge = hard_edge;
                        if (ooo_seq >= right_edge) ooo_len = 0;
                        else if (ooo_seq + ooo_len > right_edge) ooo_len = right_edge - ooo_seq;
                    }

                    if (ooo_len) {
                        uint32_t orig_seq = ooo_seq;
                        uint32_t start = ooo_seq;
                        uint32_t end = ooo_seq + ooo_len;

                        for (;;) {
                            int changed = 0;
                            for (int i = 0; i < flow->reass_count; i++) {
                                tcp_reass_seg_t *r = &flow->reass[i];
                                uint32_t rs = r->seq;
                                uint32_t re = r->end;

                                if (end <= rs || start >= re) continue;
                                if (start >= rs && end <= re) {
                                    ooo_len = 0;
                                    break;
                                }

                                if (start <= rs && end >= re) {
                                    tcp_reass_remove(flow, i);
                                    changed = 1;
                                    break;
                                }

                                if (start < rs && end > rs && end <= re) {
                                    end = rs;
                                    ooo_len = end - start;
                                    changed = 1;
                                    break;
                                }

                                if (start >= rs && start < re && end > re) {
                                    start = re;
                                    ooo_len = end - start;
                                    changed = 1;
                                    break;
                                }
                            }

                            if (!changed || !ooo_len) break;
                        }

                        if (ooo_len && flow->reass_count>= TCP_REASS_MAX_SEGS) {
                            int drop_idx = 0;
                            uint32_t best = flow->reass[0].seq;

                            for (int i = 1; i < flow->reass_count; i++) {
                                if (flow->reass[i].seq <= best) continue;
                                best = flow->reass[i].seq;
                                drop_idx = i;
                            }

                            if (start >= best) ooo_len = 0;
                            else tcp_reass_remove(flow, drop_idx);
                        }

                        if (ooo_len && flow->reass_count < TCP_REASS_MAX_SEGS) {
                            uint8_t *rx = (uint8_t *)flow->rcv_buf;
                            uint32_t cap = flow->rcv_wnd_max;
                            uint32_t offset = start - orig_seq;
                            uint32_t pos = start % cap;
                            uint32_t first = cap - pos;
                            int copied = 1;

                            if (first > ooo_len) first = ooo_len;
                            if (first && !netpkt_copyout(pkt, ooo_data + offset, rx + pos, first)) copied = 0;
                            if (copied && ooo_len > first && !netpkt_copyout(pkt, ooo_data + offset + first, rx, ooo_len - first)) copied = 0;

                            if (copied) {
                                int pos_idx = flow->reass_count;
                                while (pos_idx > 0 &&flow->reass[pos_idx - 1].seq > start) {
                                    flow->reass[pos_idx] = flow->reass[pos_idx - 1];
                                    pos_idx--;
                                }

                                flow->reass[pos_idx].seq = start;
                                flow->reass[pos_idx].end = start + ooo_len;
                                flow->reass_count++;
                                flow->rcv_ooo_used += ooo_len;
                                tcp_calc_adv_wnd_field(flow, 1);
                            }
                        }
                    }
                }

                if (fin_in){
                    flow->fin_pending = 1;
                    flow->fin_seq = fin_seq;
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
            if (!flow->delayed_ack_pending){
                flow->delayed_ack_pending = 1;
                flow->delayed_ack_timer_ms = 0;
                tcp_daemon_kick();
            } else {
                tcp_send_ack_now(flow);
            }
        } else {
            if (!flow->delayed_ack_pending){
                flow->delayed_ack_pending = 1;
                flow->delayed_ack_timer_ms = 0;
                tcp_daemon_kick();
            } else {
                tcp_send_ack_now(flow);
            }
        }
    }
    netpkt_unref(pkt);
}

void tcp_flow_on_app_read(tcp_data *flow_ctx, uint32_t bytes_read){
    if (!flow_ctx || bytes_read == 0) return;

    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow) return;

    uint32_t old_edge = flow->rcv_adv_edge;
    uint32_t old_wnd = flow->rcv_wnd;
    uint32_t old_nxt = flow->rcv_nxt;

    int advanced = tcp_reass_drain_inseq(flow);
    tcp_calc_adv_wnd_field(flow, 1);

    if (flow->state == TCP_STATE_CLOSED || flow->state == TCP_TIME_WAIT) return;

    uint32_t threshold = flow->mss ? flow->mss : TCP_DEFAULT_MSS;
    uint32_t half = flow->rcv_wnd_max >> 1;
    if (half && half < threshold) threshold = half;
    if (!threshold) threshold = 1;

    uint32_t delta = flow->rcv_adv_edge > old_edge ? flow->rcv_adv_edge - old_edge : 0;
    if (advanced || flow->rcv_nxt != old_nxt || (!old_wnd && flow->rcv_wnd) || delta >= threshold) tcp_send_ack_now(flow);
}
