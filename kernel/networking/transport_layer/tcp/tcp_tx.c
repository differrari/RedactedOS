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

static uint32_t tcp_nagle_threshold(tcp_flow_t *flow) {
    uint32_t threshold = flow && flow->mss? flow->mss : TCP_NAGLE_FLUSH_THRESHOLD;
    if (!threshold) threshold = 1;
    return threshold;
}

static uint64_t tcp_emit_data(tcp_flow_t *flow, const uint8_t *payload, uint64_t payload_len) {
    if (!flow || (!payload && payload_len)) return 0;
    if (flow->state != TCP_ESTABLISHED && flow->state != TCP_CLOSE_WAIT) return 0;

    uint64_t in_flight = flow->snd_nxt - flow->snd_una;
    uint32_t wnd = flow->snd_wnd;
    uint32_t cwnd = flow->cwnd ? flow->cwnd : (flow->mss ? flow->mss : TCP_DEFAULT_MSS);
    uint32_t eff_wnd = wnd < cwnd ? wnd : cwnd;

    uint64_t can_send = 0;
    if (in_flight < eff_wnd) can_send = eff_wnd - in_flight;

    uint64_t remaining = payload_len;
    uint64_t sent_bytes = 0;
    int first_segment = 1;

    while (remaining > 0 && can_send > 0) {
        uint64_t seg_len = remaining > can_send ? can_send : remaining;
        if (flow->mss && seg_len > flow->mss) seg_len = flow->mss;

        tcp_tx_seg_t *seg = tcp_alloc_tx_seg(flow);
        if (!seg) break;

        uintptr_t buf = (uintptr_t)zalloc(seg_len);
        if (!buf) {
            seg->used = 0;
            break;
        }

        memcpy((void*)buf, payload + sent_bytes, seg_len);

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

    flow->ctx.sequence = flow->snd_nxt;
    return sent_bytes;
}

static uint64_t tcp_nagle_append(tcp_flow_t *flow, const uint8_t *payload, uint64_t payload_len) {
    if (!flow || (!payload && payload_len) || !payload_len) return 0;
    if (flow->nagle_flushing || flow->nagle_appending) return 0;

    flow->nagle_appending = 1;

    uint32_t cap = tcp_nagle_threshold(flow);
    if (flow->nagle_len >= cap) {
        flow->nagle_appending = 0;
        return 0;
    }

    if (!flow->nagle_buf || flow->nagle_cap < cap) {
        uintptr_t nb = (uintptr_t)zalloc(cap);
        if (!nb) {
            flow->nagle_appending = 0;
            return 0;
        }

        if (flow->nagle_buf && flow->nagle_len) memcpy((void*)nb, (const void*)flow->nagle_buf, flow->nagle_len);
        if (flow->nagle_buf) release((void*)flow->nagle_buf);

        flow->nagle_buf = nb;
        flow->nagle_cap = cap;
    }

    uint64_t n = payload_len;
    uint32_t room = flow->nagle_cap - flow->nagle_len;
    if (n > room)n = room;

    memcpy((void*)(flow->nagle_buf + flow->nagle_len), payload, n);
    flow->nagle_len += (uint32_t)n;
    flow->nagle_appending = 0;
    tcp_daemon_kick();
    return n;
}

uint64_t tcp_flush_nagle(tcp_flow_t *flow, uint8_t force) {
    if (!flow || !flow->nagle_len || !flow->nagle_buf) return 0;
    if (flow->nagle_flushing || flow->nagle_appending) return 0;

    uint64_t in_flight = flow->snd_nxt - flow->snd_una;
    if (!force && in_flight && flow->nagle_len<tcp_nagle_threshold(flow)) return 0;

    flow->nagle_flushing = 1;

    uint32_t old_len = flow->nagle_len;
    uint64_t sent = tcp_emit_data(flow, (const uint8_t*)flow->nagle_buf, old_len);
    if (!sent) {
        flow->nagle_flushing = 0;
        return 0;
    }

    if (sent >= old_len) {
        release((void*)flow->nagle_buf);
        flow->nagle_buf = 0;
        flow->nagle_len = 0;
        flow->nagle_cap = 0;
        flow->nagle_timer_ms = 0;
        flow->nagle_flushing = 0;
        return sent;
    }

    memmove((void*)flow->nagle_buf, (const void*)(flow->nagle_buf + sent), old_len - sent);
    flow->nagle_len = old_len - (uint32_t)sent;
    flow->nagle_timer_ms = 0;
    flow->nagle_flushing = 0;
    tcp_daemon_kick();
    return sent;
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
    if (!flow || !seg) return;
    if (flow->state == TCP_STATE_CLOSED) return;
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

                wr_be32(&opts[o], left);
                wr_be32(&opts[o + 4], right);
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

    if (flow->nagle_len) tcp_flush_nagle(flow, 1);
    if (flow->nagle_len) return 0;

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

tcp_result_t tcp_flow_flush(tcp_data *flow_ctx){
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

    if (flow->state != TCP_ESTABLISHED && flow->state != TCP_CLOSE_WAIT && flow->state != TCP_FIN_WAIT_1 && flow->state != TCP_FIN_WAIT_2) return TCP_INVALID;

    while (flow->nagle_len) {
        uint32_t before = flow->nagle_len;
        uint64_t sent = tcp_flush_nagle(flow, 1);
        if (!sent || flow->nagle_len == before) break;
    }

    if (flow->fin_tx_pending) tcp_try_send_pending_fin(flow);
    tcp_daemon_kick();
    return flow->nagle_len ? TCP_WOULDBLOCK : TCP_OK;
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

    uint64_t accepted = 0;

    if (payload_len && flow->nagle_len) {
        uint64_t n = tcp_nagle_append(flow, payload_ptr, payload_len);
        accepted += n;
        payload_ptr += n;
        payload_len -= n;
        if (flow->nagle_len >= tcp_nagle_threshold(flow)) tcp_flush_nagle(flow, 1);
    }

    if (payload_len && payload_len < tcp_nagle_threshold(flow)) {
        uint64_t n = tcp_nagle_append(flow, payload_ptr, payload_len);
        accepted += n;
        payload_ptr += n;
        payload_len -= n;
        if (flow->nagle_len >= tcp_nagle_threshold(flow)) tcp_flush_nagle(flow, 1);
    }

    if (payload_len) {
        if (flow->nagle_len) tcp_flush_nagle(flow, 1);
        if (!flow->nagle_len) {
            uint64_t n = tcp_emit_data(flow, payload_ptr, payload_len);
            accepted += n;
            payload_ptr += n;
            payload_len -= n;
        }
    }

    if (want_fin && payload_len == 0) {
        flow->fin_tx_pending = 1;
        fin_queued = 1;
        if (flow->nagle_len) tcp_flush_nagle(flow, 1);
        tcp_try_send_pending_fin(flow);
    }

    flow_ctx->sequence = flow->snd_nxt;
    flow->ctx.sequence = flow->snd_nxt;

    tcp_daemon_kick();

    flow_ctx->payload.size = accepted;
    if (!accepted && fin_queued && flow->fin_tx_pending) return TCP_WOULDBLOCK;
    if (accepted || fin_queued) return TCP_OK;
    if (flow->snd_wnd == 0) tcp_persist_arm(flow);
    return TCP_WOULDBLOCK;
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
        if (flow->nagle_len) tcp_flow_flush(flow_ctx);
        tcp_try_send_pending_fin(flow);
        tcp_daemon_kick();
        return TCP_OK;
    }

    return TCP_INVALID;
}