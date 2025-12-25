#include "tcp_internal.h"
#include "networking/port_manager.h"
#include "net/internet_layer/ipv4.h"
#include "net/internet_layer/ipv6.h"
#include "net/internet_layer/ipv4_utils.h"
#include "net/internet_layer/ipv6_utils.h"
#include "std/memory.h"
#include "math/rng.h"
#include "syscalls/syscalls.h"
#include "../tcp.h"


static inline void tcp_update_rcv_window(tcp_flow_t *flow) {
    uint32_t maxw = flow->rcv_wnd_max;
    uint32_t used = flow->rcv_buf_used;
    uint32_t freew = maxw > used ? maxw - used : 0;

    if (freew > TCP_RECV_WINDOW) freew = TCP_RECV_WINDOW;

    flow->rcv_wnd = freew;

    uint32_t adv = freew;
    if (flow->ws_ok && flow->ws_send) adv >>= flow->ws_send;
    if (adv > 65535u) adv = 65535u;
    flow->ctx.window = (uint16_t)adv;
}

static void tcp_reass_insert(tcp_flow_t *flow, uint32_t seq, const uint8_t *data, uint32_t len) {
    if (!len) return;
    if (flow->reass_count >= TCP_REASS_MAX_SEGS) return;
    if (flow->rcv_buf_used + len > flow->rcv_wnd_max) return;

    uint32_t orig_seq = seq;
    uint32_t start = seq;
    uint32_t end = seq + len;

    for(;;){
        int changed = 0;

        for (int i = 0; i < flow->reass_count; i++){
            tcp_reass_seg_t *r = &flow->reass[i];
            uint32_t rs = r->seq;
            uint32_t re = r->end;

            if (end <= rs || start >= re) continue;
            if (start >= rs && end <= re) return;

            if (start <= rs && end >= re) {
                uint32_t olen = re - rs;

                if (r->buf && olen) free((void *)r->buf, olen);

                flow->reass[i] = flow->reass[flow->reass_count - 1];
                flow->reass[flow->reass_count - 1].seq = 0;
                flow->reass[flow->reass_count - 1].end = 0;
                flow->reass[flow->reass_count - 1].buf = 0;
                flow->reass_count--;

                flow->rcv_buf_used -= olen;
                changed = 1;
                break;
            }

            if (start < rs && end > rs && end <= re) {
                end = rs;
                len = end - start;
                changed = 1;
                break;
            }

            if (start >= rs && start < re && end > re){
                start = re;
                len = end - start;
                changed = 1;
                break;
            }
        }

        if (!changed) break;
        if (!len) return;
    }

    if (!len) return;
    if (flow->reass_count >= TCP_REASS_MAX_SEGS) return;
    if (flow->rcv_buf_used + len > flow->rcv_wnd_max) return;

    uintptr_t buf = (uintptr_t)malloc(len);
    if (!buf) return;

    uint32_t offset = start - orig_seq;
    memcpy((void *)buf, data + offset, len);

    int pos = flow->reass_count;
    while (pos > 0 && flow->reass[pos - 1].seq > start){
        flow->reass[pos] = flow->reass[pos - 1];
        pos--;
    }

    flow->reass[pos].seq = start;
    flow->reass[pos].end = start + len;
    flow->reass[pos].buf = buf;
    flow->reass_count++;

    flow->rcv_buf_used += len;

    tcp_update_rcv_window(flow);
}

static void tcp_reass_drain_inseq(tcp_flow_t *flow, port_manager_t *pm, uint8_t ifx, ip_version_t ipver, const void *src_ip_addr, const void *dst_ip_addr, uint16_t src_port, uint16_t dst_port) {
    uint32_t rcv_nxt = flow->rcv_nxt;

    for(;;){
        int idx = -1;

        for (int i = 0; i < flow->reass_count; i++){
            if (flow->reass[i].seq != rcv_nxt) continue;
            idx = i;
            break;
        }

        if (idx < 0) break;

        tcp_reass_seg_t *seg = &flow->reass[idx];
        uint32_t seg_len = seg->end - seg->seq;

        if (seg_len && pm){
            port_recv_handler_t h = port_get_handler(pm, PROTO_TCP, dst_port);
            if (h) h(ifx, ipver, src_ip_addr, dst_ip_addr, seg->buf, seg_len, src_port, dst_port);
        }

        rcv_nxt += seg_len;
        flow->rcv_buf_used -= seg_len;

        if (seg->buf && seg_len) free((void *)seg->buf, seg_len);

        flow->reass[idx] = flow->reass[flow->reass_count - 1];
        flow->reass[flow->reass_count - 1].seq = 0;
        flow->reass[flow->reass_count - 1].end = 0;
        flow->reass[flow->reass_count - 1].buf = 0;
        flow->reass_count--;
    }

    flow->rcv_nxt = rcv_nxt;
    flow->ctx.ack = rcv_nxt;

    tcp_update_rcv_window(flow);
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

void tcp_input(ip_version_t ipver, const void *src_ip_addr, const void *dst_ip_addr, uint8_t l3_id, uintptr_t ptr, uint32_t len) {
    if (len < sizeof(tcp_hdr_t)) return;

    tcp_hdr_t *hdr = (tcp_hdr_t *)ptr;

    uint16_t recv_checksum = hdr->checksum;
    hdr->checksum = 0;

    uint16_t calc;

    if (ipver == IP_VER4) calc = tcp_checksum_ipv4(hdr, (uint16_t)len, *(const uint32_t *)src_ip_addr, *(const uint32_t *)dst_ip_addr);
    else calc = tcp_checksum_ipv6(hdr, (uint16_t)len, (const uint8_t *)src_ip_addr, (const uint8_t *)dst_ip_addr);

    hdr->checksum = recv_checksum;
    if (recv_checksum != calc) return;

    uint16_t src_port = bswap16(hdr->src_port);
    uint16_t dst_port = bswap16(hdr->dst_port);
    uint32_t seq = bswap32(hdr->sequence);
    uint32_t ack = bswap32(hdr->ack);
    uint8_t flags = hdr->flags;
    uint16_t window = bswap16(hdr->window);

    uint8_t hdr_len = (uint8_t)((hdr->data_offset_reserved >> 4) * 4);
    if (len < hdr_len) return;

    uint32_t data_len = len - hdr_len;

    int idx = find_flow(dst_port, ipver, src_ip_addr, src_port);
    tcp_flow_t *flow = idx >= 0 ? &tcp_flows[idx] : NULL;

    l3_ipv4_interface_t *v4 = l3_ipv4_find_by_id(l3_id);
    l3_ipv6_interface_t *v6 = v4 ? NULL : l3_ipv6_find_by_id(l3_id);
    port_manager_t *pm = v4 ? ifmgr_pm_v4(l3_id) : (v6 ? ifmgr_pm_v6(l3_id) : NULL);

    if (!pm) return;

    uint8_t ifx = 0;
    if (v4) ifx = v4->l2 ? v4->l2->ifindex : 0;
    else if (v6) ifx = v6->l2 ? v6->l2->ifindex : 0;

    if (!flow){
        int listen_idx = find_flow(dst_port, ipver, NULL, 0);

        if ((flags & (1u << SYN_F)) && !(flags & (1u << ACK_F)) && listen_idx >= 0){
            rng_t rng;
            rng_init_random(&rng);

            tcp_flow_t *lf = &tcp_flows[listen_idx];
            tcp_flow_t *nf = tcp_alloc_flow();
            if (!nf) return;

            flow = nf;
            idx = (int)(nf - tcp_flows);

            flow->local_port = dst_port;

            flow->remote.ver = ipver;
            memset(flow->remote.ip, 0, 16);
            memcpy(flow->remote.ip, src_ip_addr, (uint64_t)(ipver == IP_VER6 ? 16 : 4));
            flow->remote.port = src_port;

            flow->local.ver = ipver;
            memset(flow->local.ip, 0, 16);
            if (ipver == IP_VER4 && v4) memcpy(flow->local.ip, &v4->ip, 4);
            else if (ipver == IP_VER6 && v6) memcpy(flow->local.ip, v6->ip, 16);
            flow->local.port = dst_port;

            flow->state = TCP_SYN_RECEIVED;
            flow->retries = TCP_SYN_RETRIES;

            tcp_parsed_opts_t pop;
            tcp_parse_options((const uint8_t *)(ptr + sizeof(tcp_hdr_t)), (uint32_t)(hdr_len > sizeof(tcp_hdr_t) ? hdr_len - sizeof(tcp_hdr_t) : 0), &pop);

            flow->ws_send = lf->ws_send;
            flow->ws_recv = 0;
            flow->ws_ok = (lf->ws_ok && pop.has_wscale) ? 1 : 0;
            if (flow->ws_ok) flow->ws_recv = pop.wscale;
            else {
                flow->ws_send = 0;
                flow->ws_recv = 0;
            }

            flow->sack_ok = (lf->sack_ok && pop.sack_permitted) ? 1 : 0;

            if (pop.has_mss && pop.mss){
                uint32_t m = pop.mss;
                uint32_t minm = ipver == IP_VER6 ? 1220u : 536u;
                uint32_t maxm = ipver == IP_VER6 ? 1440u : 1460u;
                if (m < minm) m = minm;
                if (m > maxm) m = maxm;
                flow->mss = m;
            } else flow->mss = ipver == IP_VER6 ? 1440u : 1460u;
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

            flow->rcv_wnd_max = TCP_RECV_WINDOW;
            flow->rcv_buf_used = 0;
            tcp_update_rcv_window(flow);

            flow->cwnd = flow->mss;
            flow->ssthresh = TCP_RECV_WINDOW;
            flow->dup_acks = 0;
            flow->in_fast_recovery = 0;
            flow->recover = 0;
            flow->cwnd_acc = 0;

            flow->time_wait_ms = 0;
            flow->fin_wait2_ms = 0;

            tcp_hdr_t synack_hdr;
            synack_hdr.src_port = bswap16(dst_port);
            synack_hdr.dst_port = bswap16(src_port);
            synack_hdr.sequence = bswap32(iss);
            synack_hdr.ack = bswap32(seq + 1);
            synack_hdr.flags = (uint8_t)((1u << SYN_F) | (1u << ACK_F));
            synack_hdr.window = flow->ctx.window;
            synack_hdr.urgent_ptr = 0;

            uint8_t syn_opts[40];
            uint8_t syn_opts_len = tcp_build_syn_options(syn_opts, (uint16_t)flow->mss, flow->ws_ok ? flow->ws_send : 0xffu, flow->sack_ok);

            if (ipver == IP_VER4 && v4){
                ipv4_tx_opts_t tx;
                tx.scope = IP_TX_BOUND_L3;
                tx.index = v4->l3_id;
                tcp_send_segment(IP_VER4, flow->local.ip, src_ip_addr, &synack_hdr, syn_opts, syn_opts_len, NULL, 0, (const ip_tx_opts_t *)&tx);
            } else if (ipver == IP_VER6 && v6){
                ipv6_tx_opts_t tx;
                tx.scope = IP_TX_BOUND_L3;
                tx.index = v6->l3_id;
                tcp_send_segment(IP_VER6, flow->local.ip, src_ip_addr, &synack_hdr, syn_opts, syn_opts_len, NULL, 0, (const ip_tx_opts_t *)&tx);
            }

            tcp_daemon_kick();
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

        return;
    }

    if (flow->state == TCP_TIME_WAIT){
        if (flags & (1u << RST_F)) return;

        uint32_t seg_len = data_len;

        if (flags & (1u << SYN_F)) seg_len++;
        if (flags & (1u << FIN_F)) seg_len++;

        uint32_t seg_end = seq + seg_len;

        if (seq <= flow->rcv_nxt && seg_end >= flow->rcv_nxt){
            flow->time_wait_ms = 0;
            tcp_send_ack_now(flow);
        }

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

                    if (s->buf && s->len) free((void *)s->buf, s->len);

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
            tcp_parse_options((const uint8_t *)(ptr + sizeof(tcp_hdr_t)), (uint32_t)(hdr_len > sizeof(tcp_hdr_t) ? hdr_len - sizeof(tcp_hdr_t) : 0), &pop);

            flow->ws_recv = pop.has_wscale ? pop.wscale : 0;
            flow->ws_ok = (flow->ws_send != 0) && pop.has_wscale ? 1 : 0;
            if (!flow->ws_ok) {
                flow->ws_send = 0;
                flow->ws_recv = 0;
            }

            flow->sack_ok = pop.sack_permitted ? 1 : 0;

            if (pop.has_mss && pop.mss){
                uint32_t m = pop.mss;
                uint32_t minm = ipver == IP_VER6 ? 1220u : 536u;
                uint32_t maxm = ipver == IP_VER6 ? 1440u : 1460u;
                if (m < minm) m = minm;
                if (m > maxm) m = maxm;
                flow->mss = m;
            } else {
                flow->mss = ipver == IP_VER6 ? 1440u : 1460u;
            }

            uint32_t new_wnd = window;
            if (flow->ws_ok && flow->ws_recv) new_wnd <<= flow->ws_recv;
            flow->snd_wnd = new_wnd;

            tcp_update_rcv_window(flow);

            tcp_hdr_t final_ack;
            final_ack.src_port = bswap16(flow->local_port);
            final_ack.dst_port = bswap16(flow->remote.port);
            final_ack.sequence = bswap32(flow->ctx.sequence);
            final_ack.ack = bswap32(flow->ctx.ack);
            final_ack.flags = (uint8_t)(1u << ACK_F);
            final_ack.window = flow->ctx.window;
            final_ack.urgent_ptr = 0;

            if (flow->remote.ver == IP_VER4){
                ipv4_tx_opts_t tx;
                tcp_build_tx_opts_from_local_v4(flow->local.ip, &tx);
                tcp_send_segment(IP_VER4, flow->local.ip, flow->remote.ip, &final_ack, NULL, 0, NULL, 0, (const ip_tx_opts_t *)&tx);
            } else {
                ipv6_tx_opts_t tx;
                tcp_build_tx_opts_from_local_v6(flow->local.ip, &tx);
                tcp_send_segment(IP_VER6, flow->local.ip, flow->remote.ip, &final_ack, NULL, 0, NULL, 0, (const ip_tx_opts_t *)&tx);
            }

            flow->state = TCP_ESTABLISHED;
            flow->delayed_ack_pending = 0;
            flow->delayed_ack_timer_ms = 0;
            tcp_daemon_kick();
        } else if (flags & (1u << RST_F)){
            flow->state = TCP_STATE_CLOSED;
        }

        return;

    case TCP_SYN_RECEIVED:
        if ((flags & (1u << ACK_F)) && !(flags & (1u << SYN_F)) && !(flags & (1u << RST_F)) && ack == flow->ctx.expected_ack){
            flow->ctx.sequence += 1;
            flow->snd_una = ack;
            flow->snd_nxt = flow->ctx.sequence;
            flow->state = TCP_ESTABLISHED;
            flow->delayed_ack_pending = 0;
            flow->delayed_ack_timer_ms = 0;
            flow->ctx.ack_received = ack;

            port_recv_handler_t h = port_get_handler(pm, PROTO_TCP, dst_port);
            if (h) h(ifx, ipver, src_ip_addr, dst_ip_addr, 0, 0, src_port, dst_port);

            tcp_daemon_kick();
        } else if (flags & (1u << RST_F)){
            tcp_free_flow(idx);
        }

        return;

    default:
        break;
    }

    if (flags & (1u << RST_F)) {
        tcp_free_flow(idx);
        return;
    }

        int need_ack = 0;
    int ack_immediate = 0;
    int ack_defer = 0;

    if (data_len > 0 || fin) {
        uint32_t rcv_nxt = flow->rcv_nxt;
        uint32_t rcv_wnd = flow->rcv_wnd;

        uint32_t wnd_end = rcv_nxt + rcv_wnd;
        uint32_t seg_end = seg_seq + data_len + (fin ? 1u : 0u);

        if (seg_end <= rcv_nxt || seg_seq >= wnd_end){
            need_ack = 1;
            ack_immediate = 1;
        } else if (seg_seq == rcv_nxt) {
            if (data_len){
                uintptr_t buf = (uintptr_t)malloc(data_len);
                if (buf){
                    memcpy((void *)buf, (const void *)(ptr + hdr_len), data_len);

                    port_recv_handler_t h = port_get_handler(pm, PROTO_TCP, dst_port);
                    if (h) h(ifx, ipver, src_ip_addr, dst_ip_addr, buf, data_len, src_port, dst_port);
                    else free((void *)buf, data_len);
                }

                flow->rcv_nxt += data_len;
                flow->ctx.ack = flow->rcv_nxt;
            }

            if (fin && (seg_seq + data_len) == flow->rcv_nxt) {
                flow->rcv_nxt += 1;
                flow->ctx.ack = flow->rcv_nxt;

                tcp_state_t old = flow->state;

                if (old == TCP_ESTABLISHED) flow->state =TCP_CLOSE_WAIT;
                else if (old == TCP_FIN_WAIT_1) flow->state = TCP_CLOSING;
                else if (old == TCP_FIN_WAIT_2 || old == TCP_CLOSING || old == TCP_LAST_ACK){
                    flow->state = TCP_TIME_WAIT;
                    flow->time_wait_ms = 0;
                    tcp_daemon_kick();
                }

                ack_immediate = 1;
            }

            tcp_reass_drain_inseq(flow, pm, ifx, ipver, src_ip_addr, dst_ip_addr, src_port, dst_port);

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

            tcp_update_rcv_window(flow);

            if (!ack_immediate && data_len) ack_defer = 1;
            need_ack = 1;
        } else {
            if (data_len) tcp_reass_insert(flow, seg_seq, (const uint8_t *)(ptr + hdr_len), data_len);

            if (fin){
                flow->fin_pending = 1;
                flow->fin_seq = seg_seq + data_len;
            }

            need_ack = 1;
            ack_immediate = 1;
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
}