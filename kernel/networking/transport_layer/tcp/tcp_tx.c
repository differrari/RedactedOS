#include "tcp_internal.h"

uint16_t tcp_calc_adv_wnd_field(tcp_flow_t *flow, uint8_t apply_scale) {
    if (!flow) return 0;

    uint32_t quantum = 1;
    if (apply_scale && flow->ws_ok && flow->ws_send) quantum = 1u << flow->ws_send;

    uint32_t hard_edge = flow->rcv_nxt;
    if (flow->rcv_buf && flow->rcv_wnd_max) {
        hard_edge = flow->rcv_base + flow->rcv_wnd_max;
        if (hard_edge < flow->rcv_nxt) hard_edge = flow->rcv_nxt;
    }

    if (flow->rcv_adv_edge < flow->rcv_nxt) flow->rcv_adv_edge = flow->rcv_nxt;
    uint32_t accept_edge = flow->rcv_adv_edge;
    uint32_t accept_adv = accept_edge - flow->rcv_nxt;

    uint32_t adv = accept_adv;
    if (hard_edge > accept_edge) adv = hard_edge - flow->rcv_nxt;
    if (quantum > 1) adv &= ~(quantum - 1);

    if (apply_scale && adv) {
        uint32_t threshold = flow->mss ? flow->mss : TCP_DEFAULT_MSS;
        uint32_t half = flow->rcv_wnd_max >> 1;
        if (half && half < threshold) threshold = half;
        if (!threshold) threshold = 1;
        if (adv < threshold) adv = 0;
    }

    uint32_t field = adv;
    if (!apply_scale || !flow->ws_ok || flow->ws_send == 0) {
        if (field > 65535u) field = 65535u;
        adv = field;
    } else {
        field = adv >> flow->ws_send;
        if (field > 65535u) field = 65535u;
        adv = field << flow->ws_send;
    }

    if (adv) {
        flow->rcv_wnd = adv;
        flow->rcv_adv_edge = flow->rcv_nxt + adv;
    } else {
        flow->rcv_wnd = accept_adv;
        flow->rcv_adv_edge = accept_edge;
    }

    flow->ctx.window = (uint16_t)field;
    return (uint16_t)field;
}


static void tcp_persist_arm(tcp_flow_t *flow) {
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
            s->opts_len = 0;
            s->sacked = 0;
            s->sack_retransmitted = 0;
            memset(s->opts, 0, sizeof(s->opts));
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
    if (flow) flow->keepalive_idle_ms = 0;
    tcp_hdr_t hdr;

    hdr.src_port = bswap16(flow->local_port);
    hdr.dst_port = bswap16(flow->remote.port);
    hdr.sequence = bswap32(seg->seq);
    hdr.ack = bswap32(flow->ctx.ack);

    uint8_t flags = 0;
    if (!(flow->state == TCP_SYN_SENT && seg->syn && flow->ctx.ack == 0)) flags |= (uint8_t)(1u << ACK_F);
    if (seg->syn) flags |= (uint8_t)(1u << SYN_F);
    if (seg->fin) flags |= (uint8_t)(1u << FIN_F);
    hdr.flags = flags;

    hdr.window = tcp_calc_adv_wnd_field(flow, seg->syn ? 0 : 1);
    hdr.urgent_ptr = 0;
    const uint8_t *opts = seg->opts_len ? seg->opts : NULL;

    if (flow->local.ver == IP_VER4) {
        ipv4_tx_opts_t tx;
        tcp_build_tx_opts_from_local_v4(flow->local.ip, &tx);
        (void)tcp_send_segment(IP_VER4, flow->local.ip, flow->remote.ip, &hdr, opts, seg->opts_len, seg->buf ? (const uint8_t *)seg->buf : NULL, seg->len, (const ip_tx_opts_t *)&tx, flow->ip_ttl, flow->ip_dontfrag);
    } else if (flow->local.ver == IP_VER6) {
        ipv6_tx_opts_t tx;
        tcp_build_tx_opts_from_local_v6(flow->local.ip, &tx);
        (void)tcp_send_segment(IP_VER6, flow->local.ip, flow->remote.ip, &hdr, opts, seg->opts_len, seg->buf ? (const uint8_t *)seg->buf : NULL, seg->len, (const ip_tx_opts_t *)&tx, flow->ip_ttl, flow->ip_dontfrag);
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
    ackhdr.window = tcp_calc_adv_wnd_field(flow, 1);
    ackhdr.urgent_ptr = 0;

    uint8_t opts[64];
    uint8_t opts_len = 0;

    opts_len = 0;

    if (flow->sack_ok && flow->reass_count > 0) {
        tcp_sack_block_t blocks[TCP_SACK_MAX_BLOCKS];
        uint32_t n = 0;

        if (flow->sack_recent_right > flow->sack_recent_left) {
            for (uint32_t i = 0; i < flow->reass_count; i++) {
                if (flow->reass[i].seq > flow->sack_recent_left) continue;
                if (flow->reass[i].end < flow->sack_recent_right) continue;

                blocks[n].left = flow->reass[i].seq;
                blocks[n].right = flow->reass[i].end;
                n++;
                break;
            }
        }

        for (uint32_t i = 0; i < flow->reass_count && n < TCP_SACK_MAX_BLOCKS; i++) {
            uint32_t left = flow->reass[i].seq;
            uint32_t right = flow->reass[i].end;
            int duplicate = 0;

            for (uint32_t j = 0; j < n; j++) {
                if (blocks[j].left == left && blocks[j].right == right) {
                    duplicate = 1;
                    break;
                }
            }

            if (duplicate) continue;
            blocks[n].left = left;
            blocks[n].right = right;
            n++;
        }

        uint32_t need = 2 + 8 * n;
        uint32_t pad = (4 - (need & 3)) & 3;

        if (n && need + pad <= sizeof(opts)) {
            opts[0] = 5;
            opts[1] = (uint8_t)need;
            uint32_t o = 2;

            for (uint32_t i = 0; i < n; i++) {
                uint32_t left = blocks[i].left;
                uint32_t right = blocks[i].right;

                opts[o + 0] = (uint8_t)(left >> 24);
                opts[o + 1] = (uint8_t)(left >> 16);
                opts[o + 2] = (uint8_t)(left >> 8);
                opts[o + 3] = (uint8_t)(left);
                opts[o + 4] = (uint8_t)(right >> 24);
                opts[o + 5] = (uint8_t)(right >> 16);
                opts[o + 6] = (uint8_t)(right >> 8);
                opts[o + 7] = (uint8_t)(right);
                o += 8;
            }

            for (uint32_t i = 0; i < pad; i++) opts[o + i] = 1;

            opts_len = (uint8_t)(need + pad);
        }
    }

    if (flow->local.ver == IP_VER4) {
        ipv4_tx_opts_t tx;
        tcp_build_tx_opts_from_local_v4(flow->local.ip, &tx);
        (void)tcp_send_segment(IP_VER4, flow->local.ip, flow->remote.ip, &ackhdr, opts_len ? opts : NULL, opts_len, NULL, 0, (const ip_tx_opts_t *)&tx, flow->ip_ttl, flow->ip_dontfrag);
    } else if (flow->local.ver == IP_VER6) {
        ipv6_tx_opts_t tx;
        tcp_build_tx_opts_from_local_v6(flow->local.ip, &tx);
        (void)tcp_send_segment(IP_VER6, flow->local.ip, flow->remote.ip, &ackhdr, opts_len ? opts : NULL, opts_len, NULL, 0, (const ip_tx_opts_t *)&tx, flow->ip_ttl, flow->ip_dontfrag);
    }

    flow->delayed_ack_pending = 0;
    flow->delayed_ack_timer_ms = 0;
    tcp_daemon_kick();
}

int tcp_try_send_pending_fin(tcp_flow_t *flow) {
    if (!flow || !flow->fin_tx_pending) return 0;
    if (flow->state != TCP_ESTABLISHED&&  flow->state != TCP_CLOSE_WAIT) return 0;

    uint64_t in_flight = flow->snd_nxt - flow->snd_una;
    uint32_t wnd = flow->snd_wnd;
    uint32_t cwnd = flow->cwnd ? flow->cwnd : (flow->mss ? flow->mss : TCP_DEFAULT_MSS);
    uint32_t eff_wnd = wnd < cwnd ? wnd : cwnd;

    if (eff_wnd == 0 || in_flight >= eff_wnd) return 0;
    tcp_tx_seg_t *seg = tcp_alloc_tx_seg(flow);
    if (!seg) return 0;

    seg->seq = flow->snd_nxt;
    seg->len = 0;
    seg->buf = 0;
    seg->syn = 0;
    seg->fin = 1;
    seg->timer_ms = 0;
    seg->timeout_ms = flow->rto ? flow->rto : TCP_INIT_RTO;
    seg->retransmit_cnt = 0;
    seg->rtt_sample = 0;

    tcp_send_from_seg(flow, seg);

    flow->snd_nxt += 1;
    flow->ctx.expected_ack = flow->snd_nxt;
    flow->ctx.sequence = flow->snd_nxt;
    flow->fin_tx_pending = 0;

    if (flow->state == TCP_ESTABLISHED) flow->state = TCP_FIN_WAIT_1;
    else flow->state = TCP_LAST_ACK;

    tcp_daemon_kick();
    return 1;
}

tcp_result_t tcp_flow_send(tcp_data *flow_ctx){
    if (!flow_ctx) return TCP_INVALID;

    tcp_flow_t *flow = NULL;
    for (int i = 0; i < MAX_TCP_FLOWS; i++) {
        if (!tcp_flows[i]) continue;
        if (&tcp_flows[i]->ctx == flow_ctx) {
            flow = tcp_flows[i];
            break;
        }
    }
    if (!flow) return TCP_INVALID;

    uint8_t flags = flow_ctx->flags;
    uint8_t *payload_ptr = (uint8_t *)flow_ctx->payload.ptr;
    uint64_t payload_len = flow_ctx->payload.size;
    flow_ctx->payload.size = 0;
    int want_fin = (flags & (1u << FIN_F)) != 0;
    int fin_queued = 0;

    if (flow->state != TCP_ESTABLISHED && flow->state != TCP_CLOSE_WAIT) return TCP_INVALID;

    if (flow->snd_wnd == 0 && (!want_fin || payload_len > 0)) {
        tcp_persist_arm(flow);
        return TCP_WOULDBLOCK;
    }

    uint64_t in_flight = flow->snd_nxt - flow->snd_una;
    uint32_t wnd = flow->snd_wnd;
    uint32_t cwnd = flow->cwnd ? flow->cwnd : (flow->mss ? flow->mss : TCP_DEFAULT_MSS);
    uint32_t eff_wnd = wnd < cwnd ? wnd : cwnd;

    if (in_flight >= eff_wnd && !want_fin) return TCP_WOULDBLOCK;

    uint64_t can_send = in_flight < eff_wnd ? eff_wnd - in_flight :0;
    if (can_send == 0 && !want_fin) return TCP_WOULDBLOCK;

    uint64_t remaining = payload_len;
    uint64_t sent_bytes = 0;
    int first_segment = 1;

    while (remaining > 0 && can_send > 0) {
        uint64_t seg_len = (uint64_t)(remaining > can_send ? can_send : remaining);
        if (flow->mss && seg_len > flow->mss) seg_len = (uint64_t)flow->mss;

        tcp_tx_seg_t *seg = tcp_alloc_tx_seg(flow);
        if (!seg) break;

        uintptr_t buf = 0;
        if (seg_len) {
            buf = (uintptr_t)zalloc(seg_len);
            if (!buf) { seg->used = 0; break; }
            memcpy((void *)buf, payload_ptr + sent_bytes, seg_len);
        }

        seg->seq = flow->snd_nxt;
        seg->len = seg_len;
        seg->buf = buf;
        seg->syn = 0;
        seg->fin = 0;
        seg->timer_ms = 0;
        seg->timeout_ms = flow->rto ? flow->rto : TCP_INIT_RTO;
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

    if (want_fin && remaining == 0) {
        flow->fin_tx_pending = 1;
        fin_queued = 1;
        tcp_try_send_pending_fin(flow);
    }

    flow_ctx->sequence = flow->snd_nxt;
    flow->ctx.sequence = flow->snd_nxt;

    tcp_daemon_kick();

    flow_ctx->payload.size = sent_bytes;
    if (!sent_bytes && fin_queued && flow->fin_tx_pending) return TCP_WOULDBLOCK;
    return (sent_bytes || fin_queued) ? TCP_OK : TCP_WOULDBLOCK;
}

tcp_result_t tcp_flow_close(tcp_data *flow_ctx){
    if (!flow_ctx) return TCP_INVALID;

    tcp_flow_t *flow = NULL;
    for (int i = 0; i < MAX_TCP_FLOWS; i++) {
        if (!tcp_flows[i]) continue;
        if (&tcp_flows[i]->ctx == flow_ctx) {
            flow = tcp_flows[i];
            break;
        }
    }
    if (!flow) return TCP_INVALID;

    if (flow->state == TCP_ESTABLISHED || flow->state == TCP_CLOSE_WAIT) {
        flow->fin_tx_pending = 1;
        tcp_try_send_pending_fin(flow);
        tcp_daemon_kick();
        return TCP_OK;
    }

    return TCP_INVALID;
}