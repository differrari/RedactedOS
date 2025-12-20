#include "tcp_internal.h"

static void tcp_persist_arm(tcp_flow_t *flow){
    if (!flow) return;
    flow->persist_active = 1;
    flow->persist_timer_ms = 0;
    if (flow->persist_timeout_ms == 0) flow->persist_timeout_ms = TCP_PERSIST_MIN_MS;
    if (flow->persist_timeout_ms < TCP_PERSIST_MIN_MS) flow->persist_timeout_ms = TCP_PERSIST_MIN_MS;
    if (flow->persist_timeout_ms > TCP_PERSIST_MAX_MS) flow->persist_timeout_ms = TCP_PERSIST_MAX_MS;
    tcp_daemon_kick();
}

tcp_tx_seg_t *tcp_alloc_tx_seg(tcp_flow_t *flow){
    for (int i = 0; i < TCP_MAX_TX_SEGS; i++) {
        if (!flow->txq[i].used) {
            tcp_tx_seg_t *s = &flow->txq[i];
            s->used = 1;
            s->syn = 0;
            s->fin = 0;
            s->rtt_sample = 0;
            s->retransmit_cnt = 0;
            s->seq = 0;
            s->len = 0;
            s->buf = 0;
            s->timer_ms = 0;
            s->timeout_ms = flow->rto ? flow->rto : TCP_INIT_RTO;
            tcp_daemon_kick();
            return s;
        }
    }
    return NULL;
}

void tcp_send_from_seg(tcp_flow_t *flow, tcp_tx_seg_t *seg){
    tcp_hdr_t hdr;

    hdr.src_port = bswap16(flow->local_port);
    hdr.dst_port = bswap16(flow->remote.port);
    hdr.sequence = bswap32(seg->seq);
    hdr.ack = bswap32(flow->ctx.ack);

    uint8_t flags = (uint8_t)(1u << ACK_F);
    if (seg->syn) flags |= (uint8_t)(1u << SYN_F);
    if (seg->fin) flags |= (uint8_t)(1u << FIN_F);
    hdr.flags = flags;

    hdr.window = flow->ctx.window ? flow->ctx.window : TCP_RECV_WINDOW;
    hdr.urgent_ptr = 0;

    if (flow->remote.ver == IP_VER4) {
        ipv4_tx_opts_t tx;
        tcp_build_tx_opts_from_local_v4(flow->local.ip, &tx);
        (void)tcp_send_segment(IP_VER4, flow->local.ip, flow->remote.ip, &hdr, seg->buf ? (const uint8_t *)seg->buf : NULL, seg->len, (const ip_tx_opts_t *)&tx);
    } else if (flow->remote.ver == IP_VER6) {
        ipv6_tx_opts_t tx;
        tcp_build_tx_opts_from_local_v6(flow->local.ip, &tx);
        (void)tcp_send_segment(IP_VER6, flow->local.ip, flow->remote.ip, &hdr, seg->buf ? (const uint8_t *)seg->buf : NULL, seg->len, (const ip_tx_opts_t *)&tx);
    }

    tcp_daemon_kick();
}

void tcp_send_ack_now(tcp_flow_t *flow){
    if (!flow) return;

    tcp_hdr_t ackhdr;
    ackhdr.src_port = bswap16(flow->local_port);
    ackhdr.dst_port = bswap16(flow->remote.port);
    ackhdr.sequence = bswap32(flow->ctx.sequence);
    ackhdr.ack = bswap32(flow->ctx.ack);
    ackhdr.flags = (uint8_t)(1u << ACK_F);
    ackhdr.window = flow->ctx.window ? flow->ctx.window : TCP_RECV_WINDOW;
    ackhdr.urgent_ptr = 0;

    if (flow->remote.ver == IP_VER4) {
        ipv4_tx_opts_t tx;
        tcp_build_tx_opts_from_local_v4(flow->local.ip, &tx);
        (void)tcp_send_segment(IP_VER4, flow->local.ip, flow->remote.ip, &ackhdr, NULL, 0, (const ip_tx_opts_t *)&tx);
    } else if (flow->remote.ver == IP_VER6) {
        ipv6_tx_opts_t tx;
        tcp_build_tx_opts_from_local_v6(flow->local.ip, &tx);
        (void)tcp_send_segment(IP_VER6, flow->local.ip, flow->remote.ip, &ackhdr, NULL, 0, (const ip_tx_opts_t *)&tx);
    }

    flow->delayed_ack_pending = 0;
    flow->delayed_ack_timer_ms = 0;
    tcp_daemon_kick();
}

tcp_result_t tcp_flow_send(tcp_data *flow_ctx){
    if (!flow_ctx) return TCP_INVALID;

    tcp_flow_t *flow = NULL;
    for (int i = 0; i < MAX_TCP_FLOWS; i++) {
        if (&tcp_flows[i].ctx == flow_ctx) { flow = &tcp_flows[i]; break; }
    }
    if (!flow) return TCP_INVALID;

    uint8_t flags = flow_ctx->flags;
    uint8_t *payload_ptr = (uint8_t *)flow_ctx->payload.ptr;
    uint16_t payload_len = flow_ctx->payload.size;

    if (flow->state != TCP_ESTABLISHED && !(flags & (1u << FIN_F))) {
        if (!(flow->state == TCP_CLOSE_WAIT && (flags & (1u << FIN_F)))) return TCP_INVALID;
    }

    if (flow->snd_wnd == 0 && !(flags & (1u << FIN_F))) {
        tcp_persist_arm(flow);
        return TCP_WOULDBLOCK;
    }

    uint32_t in_flight = flow->snd_nxt - flow->snd_una;
    uint32_t wnd = flow->snd_wnd;
    uint32_t cwnd = flow->cwnd ? flow->cwnd : (flow->mss ? flow->mss : TCP_DEFAULT_MSS);
    uint32_t eff_wnd = wnd < cwnd ? wnd : cwnd;

    if (eff_wnd == 0) eff_wnd = 1;
    if (in_flight >= eff_wnd && !(flags & (1u << FIN_F))) return TCP_WOULDBLOCK;

    uint32_t can_send = eff_wnd - in_flight;
    if (can_send == 0 && !(flags & (1u << FIN_F))) return TCP_WOULDBLOCK;

    uint32_t remaining = payload_len;
    uint32_t sent_bytes = 0;
    int first_segment = 1;

    while (remaining > 0 && can_send > 0) {
        uint16_t seg_len = (uint16_t)(remaining > can_send ? can_send : remaining);
        if (flow->mss && seg_len > flow->mss) seg_len = (uint16_t)flow->mss;

        tcp_tx_seg_t *seg = tcp_alloc_tx_seg(flow);
        if (!seg) break;

        uintptr_t buf = 0;
        if (seg_len) {
            buf = (uintptr_t)malloc(seg_len);
            if (!buf) { seg->used = 0; break; }
            memcpy((void *)buf, payload_ptr + sent_bytes, seg_len);
        }

        seg->seq = flow->snd_nxt;
        seg->len = seg_len;
        seg->buf = buf;
        seg->syn = 0;
        seg->fin = 0;
        seg->timer_ms = 0;
        seg->timeout_ms = flow->rto;
        seg->retransmit_cnt = 0;
        seg->rtt_sample = 0;
        if (!flow->rtt_valid && first_segment) seg->rtt_sample = 1;

        tcp_send_from_seg(flow, seg);

        flow->snd_nxt += seg_len;
        sent_bytes += seg_len;
        remaining -= seg_len;
        can_send -= seg_len;
        first_segment = 0;
    }

    if ((flags & (1u << FIN_F)) && remaining == 0) {
        tcp_tx_seg_t *seg = tcp_alloc_tx_seg(flow);
        if (!seg) return sent_bytes ? TCP_OK : TCP_WOULDBLOCK;

        seg->seq = flow->snd_nxt;
        seg->len = 0;
        seg->buf = 0;
        seg->syn = 0;
        seg->fin = 1;
        seg->timer_ms = 0;
        seg->timeout_ms = flow->rto;
        seg->retransmit_cnt = 0;
        seg->rtt_sample = 0;

        tcp_send_from_seg(flow, seg);

        flow->snd_nxt += 1;
        flow->ctx.expected_ack = flow->snd_nxt;
    }

    flow_ctx->sequence = flow->snd_nxt;
    flow->ctx.sequence = flow->snd_nxt;

    tcp_daemon_kick();

    return sent_bytes || (flags & (1u << FIN_F)) ? TCP_OK : TCP_WOULDBLOCK;
}

tcp_result_t tcp_flow_close(tcp_data *flow_ctx){
    if (!flow_ctx) return TCP_INVALID;

    tcp_flow_t *flow = NULL;
    for (int i = 0; i < MAX_TCP_FLOWS; i++) {
        if (&tcp_flows[i].ctx == flow_ctx) { flow = &tcp_flows[i]; break; }
    }
    if (!flow) return TCP_INVALID;

    if (flow->state == TCP_ESTABLISHED || flow->state == TCP_CLOSE_WAIT) {
        flow_ctx->sequence = flow->snd_nxt;
        flow_ctx->ack = flow->ctx.ack;
        flow_ctx->window = flow->ctx.window ? flow->ctx.window : TCP_RECV_WINDOW;
        flow_ctx->payload.ptr = 0;
        flow_ctx->payload.size = 0;
        flow_ctx->flags = (uint8_t)((1u << FIN_F) | (1u << ACK_F));

        tcp_result_t res = tcp_flow_send(flow_ctx);
        if (res == TCP_OK || res == TCP_WOULDBLOCK) {
            if (flow->state == TCP_ESTABLISHED) flow->state = TCP_FIN_WAIT_1;
            else flow->state = TCP_LAST_ACK;
        }
        tcp_daemon_kick();
        return res;
    }

    return TCP_INVALID;
}