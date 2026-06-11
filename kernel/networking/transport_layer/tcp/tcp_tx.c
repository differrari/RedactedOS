#include "tcp_internal.h"

uint16_t tcp_calc_adv_wnd_field(tcp_flow_t *flow, uint8_t apply_scale) {
    if (!flow) return 0;

    uint32_t shift = 0;
    if (apply_scale && flow->tx.ws_ok && flow->tx.ws_send) shift = flow->tx.ws_send;
    if (TCP_SEQ_LT(flow->rx.rcv_adv_edge, flow->rx.rcv_nxt)) flow->rx.rcv_adv_edge = flow->rx.rcv_nxt;
    uint32_t accept_edge = flow->rx.rcv_adv_edge;

    uint32_t hard_edge = flow->rx.rcv_nxt;
    if (flow->rx.rcv_buf && flow->rx.rcv_wnd_max) {
        hard_edge = flow->rx.rcv_base + flow->rx.rcv_wnd_max;
        if (TCP_SEQ_LT(hard_edge, flow->rx.rcv_nxt)) hard_edge = flow->rx.rcv_nxt;
    } else if (flow->base.state == TCP_SYN_RECEIVED && flow->rx.rcv_wnd_max) hard_edge = flow->rx.rcv_nxt + flow->rx.rcv_wnd_max;

    if (TCP_SEQ_GT(hard_edge, accept_edge)) {
        uint32_t adv = hard_edge - flow->rx.rcv_nxt;

        if (apply_scale && adv) {
            uint32_t threshold = flow->tx.mss ? flow->tx.mss : TCP_DEFAULT_MSS;
            uint32_t half = flow->rx.rcv_wnd_max >> 1;
            uint32_t already = TCP_SEQ_GT(accept_edge, flow->rx.rcv_nxt) ? accept_edge - flow->rx.rcv_nxt : 0;
            if (half && half < threshold) threshold = half;
            if (!threshold) threshold = 1;
            if (!already && adv < threshold) adv = 0;
        }

        if (adv) {
            uint32_t field = adv;
            uint32_t actual = adv;

            if (shift) {
                field = adv >> shift;
                if (field > 65535u) field = 65535u;
                actual = field << shift;
            } else if (field > 65535u) {
                field = 65535u;
                actual = field;
            }

            uint32_t new_edge = flow->rx.rcv_nxt + actual;
            if (actual && TCP_SEQ_GT(new_edge, accept_edge)) accept_edge = new_edge;
        }
    }

    if (TCP_SEQ_LT(accept_edge, flow->rx.rcv_nxt)) accept_edge = flow->rx.rcv_nxt;
    uint32_t accept_adv = accept_edge - flow->rx.rcv_nxt;
    uint32_t field = accept_adv;

    if (shift) field >>= shift;
    if (field > 65535) field = 65535;

    flow->rx.rcv_wnd = accept_adv;
    flow->rx.rcv_adv_edge = accept_edge;
    flow->base.ctx.window = (uint16_t)field;
    return (uint16_t)field;
}


static uint32_t tcp_nagle_threshold(tcp_flow_t *flow) {
    uint32_t threshold = flow && flow->tx.mss? flow->tx.mss : TCP_NAGLE_FLUSH_THRESHOLD;
    if (!threshold) threshold = 1;
    return threshold;
}

static uint64_t tcp_emit_data(tcp_flow_t *flow, const uint8_t *payload, uint64_t payload_len) {
    if (!flow || (!payload && payload_len)) return 0;
    if (flow->base.state != TCP_ESTABLISHED && flow->base.state != TCP_CLOSE_WAIT) return 0;

    uint64_t in_flight = TCP_SEQ_GT(flow->tx.snd_nxt, flow->tx.snd_una) ? flow->tx.snd_nxt - flow->tx.snd_una : 0;
    uint32_t wnd = flow->tx.snd_wnd;
    uint32_t cwnd = flow->tx.cwnd ? flow->tx.cwnd : (flow->tx.mss ? flow->tx.mss : TCP_DEFAULT_MSS);
    uint32_t eff_wnd = wnd < cwnd ? wnd : cwnd;

    uint64_t can_send = 0;
    if (in_flight < eff_wnd) can_send = eff_wnd - in_flight;

    uint64_t remaining = payload_len;
    uint64_t sent_bytes = 0;
    int first_segment = 1;

    while (remaining > 0 && can_send > 0) {
        uint32_t free_slots = 0;
        for (int i = 0; i < TCP_MAX_TX_SEGS; i++) if (!flow->tx.txq[i].used) free_slots++;
        tcp_admit_result_t tx_admit = tcp_admit_tx(flow, 0, free_slots);
        if (tx_admit != TCP_ADMIT_OK) {
            if (tx_admit == TCP_ADMIT_TX_FLOW_SEGS) tcp_stats.tx_block_flow_segs++;
            else if (tx_admit == TCP_ADMIT_TX_FLOW_BYTES) tcp_stats.tx_block_flow_bytes++;
            else if (tx_admit == TCP_ADMIT_TX_GLOBAL_BYTES) tcp_stats.tx_block_global_bytes++;
            break;
        }

        uint64_t seg_len = remaining > can_send ? can_send : remaining;
        if (flow->tx.mss && seg_len > flow->tx.mss) seg_len = flow->tx.mss;

        uint32_t tx_limit = flow->tx.queued_limit ? flow->tx.queued_limit : TCP_TX_MAX_BYTES_PER_FLOW;
        uint32_t flow_room = tx_limit > flow->tx.queued_bytes ? tx_limit - flow->tx.queued_bytes : 0;
        uint32_t global_room = TCP_TX_MAX_BYTES_GLOBAL - tcp_tx_global_bytes;
        if (seg_len > flow_room) seg_len = flow_room;
        if (seg_len > global_room) seg_len = global_room;
        if (!seg_len) break;

        tcp_tx_seg_t *seg = tcp_alloc_tx_seg(flow);
        if (!seg) break;

        uintptr_t buf = (uintptr_t)zalloc(seg_len);
        if (!buf) {
            seg->used = 0;
            break;
        }

        memcpy((void*)buf, payload + sent_bytes, seg_len);

        seg->seq = flow->tx.snd_nxt;
        seg->len = seg_len;
        seg->buf = buf;
        seg->syn = 0;
        seg->fin = 0;
        seg->timer_ms = 0;
        seg->timeout_ms = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
        seg->retransmit_cnt = 0;
        seg->rtt_sample = 0;

        if (!flow->tx.rtt_valid && first_segment) seg->rtt_sample = 1;
        tcp_account_tx_add(flow, (uint32_t)seg_len);
        flow->tx.snd_nxt += seg_len;
        flow->base.ctx.sequence = flow->tx.snd_nxt;

        if (!tcp_send_from_seg(flow, seg)) {
            flow->tx.snd_nxt -= seg_len;
            flow->base.ctx.sequence = flow->tx.snd_nxt;
            tcp_account_tx_remove(flow, (uint32_t)seg_len);
            if (seg->buf && seg->len) {
                uintptr_t seg_buf = seg->buf;
                seg->buf = 0;
                seg->len = 0;
                release((void*)seg_buf);
            }
            memset(seg, 0, sizeof(*seg));
            break;
        }

        sent_bytes += seg_len;
        remaining -= seg_len;
        can_send -= seg_len;
        first_segment = 0;
    }

    flow->base.ctx.sequence = flow->tx.snd_nxt;
    return sent_bytes;
}

static uint64_t tcp_nagle_append(tcp_flow_t *flow, const uint8_t *payload, uint64_t payload_len) {
    if (!flow || (!payload && payload_len) || !payload_len) return 0;
    if (flow->tx.nagle_flushing || flow->tx.nagle_appending) return 0;

    flow->tx.nagle_appending = 1;

    uint32_t cap = tcp_nagle_threshold(flow);
    if (flow->tx.nagle_len >= cap) {
        flow->tx.nagle_appending = 0;
        return 0;
    }

    if (!flow->tx.nagle_buf || flow->tx.nagle_cap < cap) {
        uintptr_t nb = flow->tx.nagle_buf ? (uintptr_t)reallocate((void*)flow->tx.nagle_buf, cap) : (uintptr_t)zalloc(cap);
        if (!nb) {
            flow->tx.nagle_appending = 0;
            return 0;
        }

        flow->tx.nagle_buf = nb;
        flow->tx.nagle_cap = cap;
    }

    uint64_t n = payload_len;
    uint32_t room = flow->tx.nagle_cap - flow->tx.nagle_len;
    if (n > room)n = room;

    memcpy((void*)(flow->tx.nagle_buf + flow->tx.nagle_len), payload, n);
    flow->tx.nagle_len += (uint32_t)n;
    flow->tx.nagle_appending = 0;
    tcp_daemon_kick();
    return n;
}

uint64_t tcp_flush_nagle(tcp_flow_t *flow, uint8_t force) {
    if (!flow || !flow->tx.nagle_len || !flow->tx.nagle_buf) return 0;
    if (flow->tx.nagle_flushing || flow->tx.nagle_appending) return 0;

    uint64_t in_flight = TCP_SEQ_GT(flow->tx.snd_nxt, flow->tx.snd_una) ? flow->tx.snd_nxt - flow->tx.snd_una : 0;
    if (!force && in_flight && flow->tx.nagle_len<tcp_nagle_threshold(flow)) return 0;

    flow->tx.nagle_flushing = 1;

    uint32_t old_len = flow->tx.nagle_len;
    uint64_t sent = tcp_emit_data(flow, (const uint8_t*)flow->tx.nagle_buf, old_len);
    if (!sent) {
        flow->tx.nagle_flushing = 0;
        return 0;
    }

    if (sent >= old_len) {
        release((void*)flow->tx.nagle_buf);
        flow->tx.nagle_buf = 0;
        flow->tx.nagle_len = 0;
        flow->tx.nagle_cap = 0;
        flow->tx.nagle_timer_ms = 0;
        flow->tx.nagle_flushing = 0;
        return sent;
    }

    memmove((void*)flow->tx.nagle_buf, (const void*)(flow->tx.nagle_buf + sent), old_len - sent);
    flow->tx.nagle_len = old_len - (uint32_t)sent;
    flow->tx.nagle_timer_ms = 0;
    flow->tx.nagle_flushing = 0;
    tcp_daemon_kick();
    return sent;
}

tcp_tx_seg_t *tcp_alloc_tx_seg(tcp_flow_t *flow){
    for (int i = 0; i < TCP_MAX_TX_SEGS; i++) {
        if (!flow->tx.txq[i].used) {
            tcp_tx_seg_t *s = &flow->tx.txq[i];
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
            s->timeout_ms = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
            return s;
        }
    }
    return NULL;
}

bool tcp_send_from_seg(tcp_flow_t *flow, tcp_tx_seg_t *seg){
    if (!flow || !seg) return false;
    if (flow->base.retired || flow->base.state == TCP_STATE_CLOSED) return false;
    if (flow) flow->timer.keepalive_idle_ms = 0;
    tcp_hdr_t hdr;

    hdr.src_port = bswap16(flow->base.local_port);
    hdr.dst_port = bswap16(flow->base.remote.port);
    hdr.sequence = bswap32(seg->seq);
    hdr.ack = bswap32(flow->base.ctx.ack);

    uint8_t flags = 0;
    if (!(flow->base.state == TCP_SYN_SENT && seg->syn && flow->base.ctx.ack == 0)) flags |= (uint8_t)(1u << ACK_F);
    if (seg->syn) flags |= (uint8_t)(1u << SYN_F);
    if (seg->fin) flags |= (uint8_t)(1u << FIN_F);
    hdr.flags = flags;

    hdr.window = tcp_calc_adv_wnd_field(flow, seg->syn ? 0 : 1);
    hdr.urgent_ptr = 0;
    const uint8_t *opts = seg->opts_len ? seg->opts : NULL;

    if (flow->base.local.ver == IP_VER4) {
        ip_tx_opts_t tx;
        tcp_build_tx_opts_from_local_v4(flow->base.local.ip, &tx);
        bool ok = tcp_send_segment(IP_VER4, flow->base.local.ip, flow->base.remote.ip, &hdr, opts, seg->opts_len, seg->buf ? (const uint8_t *)seg->buf : NULL, seg->len, &tx, flow->ip.ttl, flow->ip.dontfrag);
        if (ok) tcp_daemon_kick();
        return ok;
    } else if (flow->base.local.ver == IP_VER6) {
        ip_tx_opts_t tx;
        tcp_build_tx_opts_from_local_v6(flow->base.local.ip, &tx);
        bool ok = tcp_send_segment(IP_VER6, flow->base.local.ip, flow->base.remote.ip, &hdr, opts, seg->opts_len, seg->buf ? (const uint8_t *)seg->buf : NULL, seg->len, &tx, flow->ip.ttl, flow->ip.dontfrag);
        if (ok) tcp_daemon_kick();
        return ok;
    }

    return false;
}

void tcp_send_ack_now(tcp_flow_t *flow){
    if (!flow) return;

    tcp_hdr_t ackhdr;
    ackhdr.src_port = bswap16(flow->base.local_port);
    ackhdr.dst_port = bswap16(flow->base.remote.port);
    ackhdr.sequence = bswap32(flow->base.ctx.sequence);
    ackhdr.ack = bswap32(flow->base.ctx.ack);
    ackhdr.flags = (uint8_t)(1u << ACK_F);
    ackhdr.window = tcp_calc_adv_wnd_field(flow, 1);
    ackhdr.urgent_ptr = 0;

    uint8_t opts[64];
    uint8_t opts_len = 0;

    opts_len = 0;

    if (flow->tx.sack_ok && flow->rx.reass_count > 0) {
        tcp_sack_block_t blocks[TCP_SACK_MAX_BLOCKS];
        uint32_t n = 0;

        if (flow->rx.sack_recent_right > flow->rx.sack_recent_left) {
            for (uint32_t i = 0; i < flow->rx.reass_count; i++) {
                if (TCP_SEQ_GT(flow->rx.reass[i].seq, flow->rx.sack_recent_left)) continue;
                if (TCP_SEQ_LT(flow->rx.reass[i].end, flow->rx.sack_recent_right)) continue;

                blocks[n].left = flow->rx.reass[i].seq;
                blocks[n].right = flow->rx.reass[i].end;
                n++;
                break;
            }
        }

        for (uint32_t i = 0; i < flow->rx.reass_count && n < TCP_SACK_MAX_BLOCKS; i++) {
            uint32_t left = flow->rx.reass[i].seq;
            uint32_t right = flow->rx.reass[i].end;
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

    if (flow->base.local.ver == IP_VER4) {
        ip_tx_opts_t tx;
        tcp_build_tx_opts_from_local_v4(flow->base.local.ip, &tx);
        (void)tcp_send_segment(IP_VER4, flow->base.local.ip, flow->base.remote.ip, &ackhdr, opts_len ? opts : NULL, opts_len, NULL, 0, &tx, flow->ip.ttl, flow->ip.dontfrag);
    } else if (flow->base.local.ver == IP_VER6) {
        ip_tx_opts_t tx;
        tcp_build_tx_opts_from_local_v6(flow->base.local.ip, &tx);
        (void)tcp_send_segment(IP_VER6, flow->base.local.ip, flow->base.remote.ip, &ackhdr, opts_len ? opts : NULL, opts_len, NULL, 0, &tx, flow->ip.ttl, flow->ip.dontfrag);
    }

    flow->timer.delayed_ack_pending = 0;
    flow->timer.delayed_ack_timer_ms = 0;
}

int tcp_try_send_pending_fin(tcp_flow_t *flow) {
    if (!flow || !flow->tx.fin_tx_pending) return 0;
    if (flow->base.state != TCP_ESTABLISHED&&  flow->base.state != TCP_CLOSE_WAIT) return 0;

    if (flow->tx.nagle_len) tcp_flush_nagle(flow, 1);
    if (flow->tx.nagle_len) return 0;

    uint64_t in_flight = TCP_SEQ_GT(flow->tx.snd_nxt, flow->tx.snd_una) ? flow->tx.snd_nxt - flow->tx.snd_una : 0;
    uint32_t wnd = flow->tx.snd_wnd;
    uint32_t cwnd = flow->tx.cwnd ? flow->tx.cwnd : (flow->tx.mss ? flow->tx.mss : TCP_DEFAULT_MSS);
    uint32_t eff_wnd = wnd < cwnd ? wnd : cwnd;

    if (eff_wnd == 0 || in_flight >= eff_wnd) return 0;
    tcp_tx_seg_t *seg = tcp_alloc_tx_seg(flow);
    if (!seg) return 0;

    seg->seq = flow->tx.snd_nxt;
    seg->len = 0;
    seg->buf = 0;
    seg->syn = 0;
    seg->fin = 1;
    seg->timer_ms = 0;
    seg->timeout_ms = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
    seg->retransmit_cnt = 0;
    seg->rtt_sample = 0;

    tcp_state_t old_state = flow->base.state;
    flow->tx.snd_nxt += 1;
    flow->base.ctx.expected_ack = flow->tx.snd_nxt;
    flow->base.ctx.sequence = flow->tx.snd_nxt;
    flow->tx.fin_tx_pending = 0;

    if (old_state == TCP_ESTABLISHED) flow->base.state = TCP_FIN_WAIT_1;
    else flow->base.state = TCP_LAST_ACK;

    if (!tcp_send_from_seg(flow, seg)) {
        flow->tx.snd_nxt -= 1;
        flow->base.ctx.expected_ack = flow->tx.snd_nxt;
        flow->base.ctx.sequence = flow->tx.snd_nxt;
        flow->base.state = old_state;
        flow->tx.fin_tx_pending = 1;
        memset(seg, 0, sizeof(*seg));
        tcp_daemon_kick();
        return 0;
    }

    tcp_daemon_kick();
    return 1;
}

tcp_result_t tcp_flow_flush(tcp_data *flow_ctx){
    if (!flow_ctx) return TCP_INVALID;

    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow) return TCP_INVALID;

    tcp_result_t rc = TCP_OK;
    if (flow->base.state != TCP_ESTABLISHED && flow->base.state != TCP_CLOSE_WAIT && flow->base.state != TCP_FIN_WAIT_1 && flow->base.state != TCP_FIN_WAIT_2) rc = TCP_INVALID;

    while (rc == TCP_OK && flow->tx.nagle_len) {
        uint32_t before = flow->tx.nagle_len;
        uint64_t sent = tcp_flush_nagle(flow, 1);
        if (!sent || flow->tx.nagle_len == before) break;
    }

    if (rc == TCP_OK) {
        if (flow->tx.fin_tx_pending) tcp_try_send_pending_fin(flow);
        if (flow->tx.nagle_len) {
            tcp_daemon_kick();
            rc = TCP_WOULDBLOCK;
        }
    }

    tcp_flow_put(flow);
    return rc;
}

tcp_result_t tcp_flow_send(tcp_data *flow_ctx){
    if (!flow_ctx) return TCP_INVALID;

    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow) return TCP_INVALID;

    uint8_t flags = flow_ctx->flags;
    uint8_t *payload_ptr = (uint8_t *)flow_ctx->payload.ptr;
    uint64_t payload_len = flow_ctx->payload.size;
    flow_ctx->payload.size = 0;
    int want_fin = (flags & (1u << FIN_F)) != 0;
    int fin_queued = 0;
    uint64_t accepted = 0;
    tcp_result_t rc = TCP_OK;

    if (flow->base.state != TCP_ESTABLISHED && flow->base.state != TCP_CLOSE_WAIT) rc = TCP_INVALID;

    if (rc == TCP_OK && !flow->tx.nodelay && payload_len && flow->tx.nagle_len) {
        uint64_t n = tcp_nagle_append(flow, payload_ptr, payload_len);
        accepted += n;
        payload_ptr += n;
        payload_len -= n;
        if (flow->tx.nagle_len >= tcp_nagle_threshold(flow)) tcp_flush_nagle(flow, 1);
    }

    if (rc == TCP_OK && !flow->tx.nodelay && payload_len && payload_len < tcp_nagle_threshold(flow)) {
        uint64_t n = tcp_nagle_append(flow, payload_ptr, payload_len);
        accepted += n;
        payload_ptr += n;
        payload_len -= n;
        if (flow->tx.nagle_len >= tcp_nagle_threshold(flow)) tcp_flush_nagle(flow, 1);
    }

    if (rc == TCP_OK && payload_len) {
        if (flow->tx.nagle_len) tcp_flush_nagle(flow, 1);
        if (!flow->tx.nagle_len) {
            uint64_t n = tcp_emit_data(flow, payload_ptr, payload_len);
            accepted += n;
            payload_ptr += n;
            payload_len -= n;
        }
    }

    if (rc == TCP_OK && want_fin && payload_len == 0) {
        flow->tx.fin_tx_pending = 1;
        fin_queued = 1;
        if (flow->tx.nagle_len) tcp_flush_nagle(flow, 1);
        tcp_try_send_pending_fin(flow);
    }

    flow_ctx->sequence = flow->tx.snd_nxt;
    flow->base.ctx.sequence = flow->tx.snd_nxt;

    flow_ctx->payload.size = accepted;
    if (rc == TCP_OK) {
        if (!accepted && fin_queued && flow->tx.fin_tx_pending) rc = TCP_WOULDBLOCK;
        else if (accepted || fin_queued) rc = TCP_OK;
        else { 
            if (flow->tx.snd_wnd == 0) {
                flow->timer.persist_active = 1;
                flow->timer.persist_timer_ms = 0;
                if (flow->timer.persist_timeout_ms == 0) flow->timer.persist_timeout_ms = TCP_PERSIST_MIN_MS;
                if (flow->timer.persist_timeout_ms < TCP_PERSIST_MIN_MS) flow->timer.persist_timeout_ms = TCP_PERSIST_MIN_MS;
                if (flow->timer.persist_timeout_ms > TCP_PERSIST_MAX_MS) flow->timer.persist_timeout_ms = TCP_PERSIST_MAX_MS;
                tcp_daemon_kick();
            }
            rc = TCP_WOULDBLOCK;
        }
    }

    tcp_flow_put(flow);
    return rc;
}

tcp_result_t tcp_flow_close(tcp_data *flow_ctx){
    if (!flow_ctx) return TCP_INVALID;

    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow) return TCP_INVALID;

    tcp_result_t rc = TCP_INVALID;
    if (flow->base.state == TCP_ESTABLISHED || flow->base.state == TCP_CLOSE_WAIT) {
        flow->tx.fin_tx_pending = 1;
        if (flow->tx.nagle_len) tcp_flush_nagle(flow, 1);
        tcp_try_send_pending_fin(flow);
        tcp_daemon_kick();
        rc = TCP_OK;
    }

    tcp_flow_put(flow);
    return rc;
}