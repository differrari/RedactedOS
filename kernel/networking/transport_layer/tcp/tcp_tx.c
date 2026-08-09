#include "tcp_internal.h"

const uint8_t *tcp_tx_seg_payload_ptr(const tcp_tx_seg_t *seg) {
    if (!seg || !seg->pkt || !seg->len) return NULL;
    
    uint32_t pkt_len = netpkt_len(seg->pkt);
    if (seg->payload_off > pkt_len) return NULL;
    if (seg->len > pkt_len - seg->payload_off) return NULL;

    return (const uint8_t*)(netpkt_data(seg->pkt) + (uintptr_t)seg->payload_off);
}

void tcp_tx_seg_clear(tcp_flow_t *flow, tcp_tx_seg_t *seg) {
    if (!seg) return;

    uint32_t len = seg->len;
    if (seg->rtt_sample && flow) flow->tx.rtt_sample_pending = 0;
    if (seg->pkt) netpkt_unref(seg->pkt);
    if (len && flow) tcp_account_tx_remove(flow, len);

    memset(seg, 0, sizeof(*seg));
}

void tcp_update_adv_wnd(tcp_flow_t *flow, uint8_t apply_scale) {
    if (!flow) return;

    uint32_t shift = 0;
    if (apply_scale && flow->tx.ws_ok && flow->tx.ws_send) shift = flow->tx.ws_send;
    if (TCP_SEQ_LT(flow->rx.rcv_adv_edge, flow->rx.rcv_nxt)) flow->rx.rcv_adv_edge = flow->rx.rcv_nxt;
    uint32_t accept_edge = flow->rx.rcv_adv_edge;

    uint32_t hard_edge = flow->rx.rcv_nxt;
    if (flow->rx.rcv_buf && flow->rx.rcv_wnd_max) {
        hard_edge = flow->rx.rcv_base + flow->rx.rcv_wnd_max;
        if (TCP_SEQ_LT(hard_edge, flow->rx.rcv_nxt)) hard_edge = flow->rx.rcv_nxt;
    } else if ((flow->base.state == TCP_SYN_SENT || flow->base.state == TCP_SYN_RECEIVED) && flow->rx.rcv_wnd_max) hard_edge = flow->rx.rcv_nxt + flow->rx.rcv_wnd_max;

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
}


static uint32_t tcp_nagle_threshold(tcp_flow_t *flow) {
    uint32_t threshold = flow && flow->tx.mss? flow->tx.mss : TCP_NAGLE_FLUSH_THRESHOLD;
    if (!threshold) threshold = 1;
    return threshold;
}

static uint64_t tcp_emit_data(tcp_flow_t *flow, const uint8_t *payload, uint64_t payload_len, uint8_t push_partial) {
    if (!flow || (!payload && payload_len)) return 0;
    if (flow->base.state != TCP_ESTABLISHED && flow->base.state != TCP_CLOSE_WAIT) return 0;

    uint64_t in_flight = TCP_SEQ_GT(flow->tx.snd_nxt, flow->tx.snd_una) ? flow->tx.snd_nxt - flow->tx.snd_una : 0;
    if (flow->tx.data_tx_valid && !in_flight) {
        uint32_t idle_ms = (uint32_t)get_time() - flow->tx.last_data_tx_ms;
        uint32_t rto = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
        if (idle_ms >= rto) {
            uint32_t iw = tcp_initial_cwnd(flow->tx.mss);
            if (flow->tx.cwnd > iw) flow->tx.cwnd = iw;
            flow->tx.cwnd_acc = 0;
        }
    }

    uint32_t wnd = flow->tx.snd_wnd;
    uint32_t cwnd = flow->tx.cwnd ? flow->tx.cwnd : (flow->tx.mss ? flow->tx.mss : TCP_DEFAULT_MSS);
    uint32_t eff_wnd = wnd < cwnd ? wnd : cwnd;

    uint64_t can_send = 0;
    if (in_flight < eff_wnd) can_send = eff_wnd - in_flight;
    bool persist_only = !can_send && !wnd && !in_flight && payload_len;
    if (persist_only) can_send = 1;

    uint64_t remaining = payload_len;
    uint64_t sent_bytes = 0;

    while (remaining > 0 && can_send > 0) {
        uint64_t sendable = remaining > can_send ? can_send : remaining;
        if (flow->tx.mss && sendable > flow->tx.mss) sendable = flow->tx.mss;
        if (sendable > UINT32_MAX) sendable = UINT32_MAX;
        uint32_t seg_len = (uint32_t)sendable;

        uint32_t tx_limit = flow->tx.queued_limit ? flow->tx.queued_limit : TCP_TX_MAX_BYTES_PER_FLOW;
        uint32_t flow_room = tx_limit > flow->tx.queued_bytes ? tx_limit - flow->tx.queued_bytes : 0;
        uint32_t global_room = tcp_tx_global_bytes < TCP_TX_MAX_BYTES_GLOBAL ? TCP_TX_MAX_BYTES_GLOBAL - tcp_tx_global_bytes : 0;
        if (!flow_room) {
            tcp_stats.tx_block_flow_bytes++;
            break;
        }
        if (!global_room) {
            tcp_stats.tx_block_global_bytes++;
            break;
        }
        if (seg_len > flow_room) seg_len = flow_room;
        if (seg_len > global_room) seg_len = global_room;
        if (!seg_len) break;

        tcp_tx_seg_t *seg = tcp_alloc_tx_seg(flow, TCP_TX_CONTROL_RESERVE_SEGS);
        if (!seg) {
            tcp_stats.tx_block_flow_segs++;
            break;
        }

        netpkt_t *payload_pkt = netpkt_alloc(seg_len, 0, 0);
        uint8_t *payload_dst = payload_pkt ? (uint8_t*)netpkt_put(payload_pkt, seg_len) : NULL;
        if (!payload_dst) {
            if (payload_pkt) netpkt_unref(payload_pkt);
            tcp_tx_seg_clear(flow, seg);
            break;
        }

        memcpy(payload_dst, payload + sent_bytes, seg_len);

        seg->seq = flow->tx.snd_nxt;
        seg->len = seg_len;
        seg->pkt = payload_pkt;
        seg->payload_off = 0;
        seg->syn = 0;
        seg->fin = 0;
        seg->psh = push_partial && seg_len == remaining;
        seg->persist = persist_only;
        seg->timer_ms = 0;
        seg->timeout_ms = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
        seg->retransmit_cnt = 0;
        seg->rtt_sample = 0;

        if (!persist_only && !flow->tx.rtt_sample_pending) {
            seg->rtt_sample = 1;
            flow->tx.rtt_sample_pending = 1;
        }
        tcp_account_tx_add(flow, seg_len);
        flow->tx.snd_nxt += seg_len;
        flow->base.ctx.sequence = flow->tx.snd_nxt;

        if (!tcp_send_from_seg(flow, seg)) {
            flow->tx.snd_nxt -= seg_len;
            flow->base.ctx.sequence = flow->tx.snd_nxt;
            tcp_tx_seg_clear(flow, seg);
            break;
        }

        sent_bytes += seg_len;
        remaining -= seg_len;
        can_send -= seg_len;
        if (persist_only) {
            flow->timer.persist_active = 1;
            flow->timer.persist_timer_ms = 0;
            if (!flow->timer.persist_timeout_ms) {
                uint32_t timeout = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
                if (timeout < TCP_PERSIST_MIN_MS) timeout = TCP_PERSIST_MIN_MS;
                if (timeout > TCP_PERSIST_MAX_MS) timeout = TCP_PERSIST_MAX_MS;
                flow->timer.persist_timeout_ms = timeout;
            }
            tcp_daemon_kick();
            break;
        }
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
    uint64_t sent = tcp_emit_data(flow, (const uint8_t*)flow->tx.nagle_buf, old_len, flow->tx.nagle_psh);
    if (!sent) {
        flow->tx.nagle_flushing = 0;
        return 0;
    }

    if (sent >= old_len) {
        flow->tx.nagle_len = 0;
        flow->tx.nagle_timer_ms = 0;
        flow->tx.nagle_psh = 0;
        flow->tx.nagle_flushing = 0;
        return sent;
    }

    memmove((void*)flow->tx.nagle_buf, (const void*)(flow->tx.nagle_buf + sent), old_len - sent);
    flow->tx.nagle_len = old_len - (uint32_t)sent;
    flow->tx.nagle_timer_ms = 0;
    flow->tx.nagle_flushing = 0;
    return sent;
}

tcp_tx_seg_t *tcp_alloc_tx_seg(tcp_flow_t *flow, uint32_t reserve_slots){
    if (!flow || reserve_slots >= TCP_MAX_TX_SEGS) return NULL;

    tcp_tx_seg_t *seg = NULL;
    uint32_t available = 0;
    for (uint32_t i = 0; i < TCP_MAX_TX_SEGS; i++) {
        if (flow->tx.txq[i].used) continue;
        if (!seg) seg = &flow->tx.txq[i];
        if (++available > reserve_slots) break;
    }

    if (!seg || available <= reserve_slots) return NULL;
    memset(seg, 0, sizeof(*seg));
    seg->used = 1;
    seg->timeout_ms = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
    return seg;
}

bool tcp_send_flow_segment(tcp_flow_t *flow, tcp_hdr_t *hdr, const uint8_t *opts, uint8_t opts_len, const uint8_t *payload, uint16_t payload_len) {
    if (!flow || !hdr || !flow->base.l3_id) return false;
    if (flow->base.local.ver != IP_VER4 && flow->base.local.ver != IP_VER6) return false;
    if (flow->base.remote.ver != flow->base.local.ver) return false;

    ip_tx_opts_t tx;
    tx.scope = IP_TX_BOUND_L3;
    tx.index = flow->base.l3_id;
    return tcp_send_segment(flow->base.local.ver, flow->base.local.ip, flow->base.remote.ip, hdr, opts, opts_len, payload, payload_len, &tx, flow->ip.ttl, flow->ip.dontfrag);
}

bool tcp_send_from_seg(tcp_flow_t *flow, tcp_tx_seg_t *seg){
    if (!flow || !seg) return false;
    if (flow->base.retired || flow->base.state == TCP_STATE_CLOSED) return false;
    tcp_hdr_t hdr;

    hdr.src_port = bswap16(flow->base.local.port);
    hdr.dst_port = bswap16(flow->base.remote.port);
    hdr.sequence = bswap32(seg->seq);
    hdr.ack = bswap32(flow->base.ctx.ack);

    uint8_t flags = 0;
    if (!(flow->base.state == TCP_SYN_SENT && seg->syn && flow->base.ctx.ack == 0)) flags |= (uint8_t)(1u << ACK_F);
    if (seg->syn) flags |= (uint8_t)(1u << SYN_F);
    if (seg->fin) flags |= (uint8_t)(1u << FIN_F);
    if (seg->psh && seg->len) flags |= (uint8_t)(1u << PSH_F);
    hdr.flags = flags;

    tcp_update_adv_wnd(flow, seg->syn ? 0 : 1);
    hdr.window = flow->base.ctx.window;
    hdr.urgent_ptr = 0;
    const uint8_t *opts = seg->opts_len ? seg->opts : NULL;

    bool arm_timer = !seg->timer_ms && !seg->retransmit_cnt;
    bool ok = tcp_send_flow_segment(flow, &hdr, opts, seg->opts_len, tcp_tx_seg_payload_ptr(seg), (uint16_t)seg->len);
    if (!ok) return false;
    flow->timer.keepalive_idle_ms = 0;
    if (seg->len) {
        flow->tx.data_tx_valid = 1;
        flow->tx.last_data_tx_ms = (uint32_t)get_time();
    }
    if (arm_timer) tcp_daemon_kick();
    return true;
}

bool tcp_retransmit_seg(tcp_flow_t *flow, tcp_tx_seg_t *seg) {
    if (!flow || !seg || !seg->used) return false;
    if (!tcp_send_from_seg(flow, seg)) return false;

    for (uint32_t i = 0; i < TCP_MAX_TX_SEGS; i++) flow->tx.txq[i].rtt_sample = 0;
    flow->tx.rtt_sample_pending = 0;

    if (seg->retransmit_cnt < UINT8_MAX) seg->retransmit_cnt++;
    seg->timer_ms = 0;
    seg->rtt_timer_ms = 0;
    seg->timeout_ms = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
    return true;
}

void tcp_send_ack_now(tcp_flow_t *flow){
    if (!flow) return;

    tcp_hdr_t ackhdr;
    ackhdr.src_port = bswap16(flow->base.local.port);
    ackhdr.dst_port = bswap16(flow->base.remote.port);
    ackhdr.sequence = bswap32(flow->base.ctx.sequence);
    ackhdr.ack = bswap32(flow->base.ctx.ack);
    ackhdr.flags = (uint8_t)(1u << ACK_F);
    tcp_update_adv_wnd(flow, 1);
    ackhdr.window = flow->base.ctx.window;
    ackhdr.urgent_ptr = 0;

    uint8_t opts[64];
    uint8_t opts_len = 0;

    if (flow->tx.sack_ok && (flow->rx.reass_count > 0 || (flow->tx.dsack_enabled && flow->rx.dsack_pending))) {
        tcp_sack_block_t blocks[TCP_SACK_MAX_BLOCKS];
        uint32_t n = 0;

        if (flow->tx.dsack_enabled && flow->rx.dsack_pending && TCP_SEQ_GT(flow->rx.dsack_right, flow->rx.dsack_left)) {
            blocks[n].left = flow->rx.dsack_left;
            blocks[n].right = flow->rx.dsack_right;
            n++;
        }

        if (n < TCP_SACK_MAX_BLOCKS && TCP_SEQ_GT(flow->rx.sack_recent_right, flow->rx.sack_recent_left)) {
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
            bool duplicate = false;

            for (uint32_t j = 0; j < n; j++) {
                if (blocks[j].left == left && blocks[j].right == right) {
                    duplicate = true;
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

    if (!tcp_send_flow_segment(flow, &ackhdr, opts_len ? opts : NULL, opts_len, NULL, 0)) {
        flow->timer.delayed_ack_pending = 1;
        flow->timer.delayed_ack_timer_ms = 0;
        tcp_daemon_kick();
        return;
    }

    flow->rx.dsack_pending = 0;
    flow->rx.dsack_left = 0;
    flow->rx.dsack_right = 0;
    flow->timer.delayed_ack_pending = 0;
    flow->timer.delayed_ack_timer_ms = 0;
}

void tcp_try_send_pending_fin(tcp_flow_t *flow) {
    if (!flow || !flow->tx.fin_tx_pending) return;

    tcp_state_t next_state;
    switch (flow->base.state) {
        case TCP_ESTABLISHED:
            next_state = TCP_FIN_WAIT_1;
            break;
        case TCP_CLOSE_WAIT:
            next_state = TCP_LAST_ACK;
            break;
        default:
            return;
    }

    if (flow->tx.nagle_len && flow->tx.snd_wnd == 0) return;
    if (flow->tx.nagle_len) {
        uint64_t flushed = tcp_flush_nagle(flow, 1);
        if (!flushed || flow->tx.nagle_len) return;
    }

    uint64_t in_flight = TCP_SEQ_GT(flow->tx.snd_nxt, flow->tx.snd_una) ? flow->tx.snd_nxt - flow->tx.snd_una : 0;
    uint32_t wnd = flow->tx.snd_wnd;
    uint32_t cwnd = flow->tx.cwnd ? flow->tx.cwnd : (flow->tx.mss ? flow->tx.mss : TCP_DEFAULT_MSS);
    uint32_t eff_wnd = wnd < cwnd ? wnd : cwnd;

    if (eff_wnd == 0 || in_flight >= eff_wnd) return;
    tcp_tx_seg_t *seg = tcp_alloc_tx_seg(flow, 0);
    if (!seg) return;

    seg->seq = flow->tx.snd_nxt;
    seg->len = 0;
    seg->pkt = NULL;
    seg->payload_off = 0;
    seg->syn = 0;
    seg->fin = 1;
    seg->psh = 0;
    seg->persist = 0;
    seg->timer_ms = 0;
    seg->timeout_ms = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
    seg->retransmit_cnt = 0;
    seg->rtt_sample = 0;
    if (!flow->tx.rtt_sample_pending) {
        seg->rtt_sample = 1;
        flow->tx.rtt_sample_pending = 1;
    }

    tcp_state_t old_state = flow->base.state;
    flow->tx.snd_nxt += 1;
    flow->base.ctx.expected_ack = flow->tx.snd_nxt;
    flow->base.ctx.sequence = flow->tx.snd_nxt;
    flow->tx.fin_tx_pending = 0;

    flow->base.state = next_state;

    if (!tcp_send_from_seg(flow, seg)) {
        flow->tx.snd_nxt -= 1;
        flow->base.ctx.expected_ack = flow->tx.snd_nxt;
        flow->base.ctx.sequence = flow->tx.snd_nxt;
        flow->base.state = old_state;
        flow->tx.fin_tx_pending = 1;
        tcp_tx_seg_clear(flow, seg);
        return;
    }
}

tcp_result_t tcp_flow_flush(tcp_data *flow_ctx){
    if (!flow_ctx) return TCP_INVALID;

    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow) return TCP_INVALID;

    tcp_result_t rc = TCP_OK;
    if (flow->base.state != TCP_ESTABLISHED && flow->base.state != TCP_CLOSE_WAIT && flow->base.state != TCP_FIN_WAIT_1 && flow->base.state != TCP_FIN_WAIT_2) rc = TCP_INVALID;

    while (rc == TCP_OK && flow->tx.nagle_len && flow->tx.snd_wnd > 0) {
        uint32_t before = flow->tx.nagle_len;
        uint64_t sent = tcp_flush_nagle(flow, 1);
        if (!sent || flow->tx.nagle_len == before) break;
    }

    if (rc == TCP_OK) {
        if (flow->tx.fin_tx_pending) {
            tcp_try_send_pending_fin(flow);
            if (flow->tx.fin_tx_pending) tcp_daemon_kick();
        }
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
    if (payload_len && !payload_ptr) {
        tcp_flow_put(flow);
        return TCP_INVALID;
    }
    bool want_fin = (flags & (1u << FIN_F)) != 0;
    bool want_push = (flags & (1u << PSH_F)) != 0;
    bool fin_queued = false;
    uint64_t accepted = 0;
    tcp_result_t rc = TCP_OK;
    bool zero_window_hold = false;

    if (flow->base.state != TCP_ESTABLISHED && flow->base.state != TCP_CLOSE_WAIT) rc = TCP_INVALID;

    if (rc == TCP_OK && payload_len && flow->tx.snd_wnd == 0 && flow->tx.snd_nxt == flow->tx.snd_una) {
        zero_window_hold = true;
        if (!flow->tx.nagle_len) {
            uint64_t n = tcp_nagle_append(flow, payload_ptr, 1);
            if (n && want_push && n == payload_len) flow->tx.nagle_psh = 1;
            accepted += n;
            payload_ptr += n;
            payload_len -= n;
            if (n) {
                if (!flow->timer.persist_active) {
                    flow->timer.persist_active = 1;
                    flow->timer.persist_timer_ms = 0;
                }
                if (!flow->timer.persist_timeout_ms) {
                    uint32_t timeout = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
                    if (timeout < TCP_PERSIST_MIN_MS) timeout = TCP_PERSIST_MIN_MS;
                    if (timeout > TCP_PERSIST_MAX_MS) timeout = TCP_PERSIST_MAX_MS;
                    flow->timer.persist_timeout_ms = timeout;
                }
                tcp_daemon_kick();
            }
        }
    }

    if (rc == TCP_OK && !zero_window_hold && !flow->tx.nodelay && payload_len && flow->tx.nagle_len) {
        uint64_t n = tcp_nagle_append(flow, payload_ptr, payload_len);
        if (n && want_push && n == payload_len) flow->tx.nagle_psh = 1;
        accepted += n;
        payload_ptr += n;
        payload_len -= n;
        if (flow->tx.nagle_len >= tcp_nagle_threshold(flow) && !tcp_flush_nagle(flow, 1)) tcp_daemon_kick();
    }

    uint64_t in_flight = TCP_SEQ_GT(flow->tx.snd_nxt, flow->tx.snd_una) ? flow->tx.snd_nxt - flow->tx.snd_una : 0;
    if (rc == TCP_OK && !zero_window_hold && !flow->tx.nodelay && payload_len && payload_len < tcp_nagle_threshold(flow) && in_flight != 0) {
        uint64_t n = tcp_nagle_append(flow, payload_ptr, payload_len);
        if (n && want_push && n == payload_len) flow->tx.nagle_psh = 1;
        accepted += n;
        payload_ptr += n;
        payload_len -= n;
        if (flow->tx.nagle_len >= tcp_nagle_threshold(flow) && !tcp_flush_nagle(flow, 1)) tcp_daemon_kick();
    }

    if (rc == TCP_OK && !zero_window_hold && payload_len) {
        if (flow->tx.nagle_len && !tcp_flush_nagle(flow, 1)) tcp_daemon_kick();
        if (!flow->tx.nagle_len) {
            uint64_t n = tcp_emit_data(flow, payload_ptr, payload_len, want_push ? 1 : 0);
            accepted += n;
            payload_len -= n;
        }
    }

    if (rc == TCP_OK && want_fin && payload_len == 0) {
        flow->tx.fin_tx_pending = 1;
        if (flow->tx.nagle_len) flow->tx.nagle_psh = 1;
        fin_queued = true;
        if (flow->tx.nagle_len && flow->tx.snd_wnd > 0 && !tcp_flush_nagle(flow, 1)) tcp_daemon_kick();
        tcp_try_send_pending_fin(flow);
        if (flow->tx.fin_tx_pending) tcp_daemon_kick();
    }

    flow_ctx->sequence = flow->tx.snd_nxt;
    flow->base.ctx.sequence = flow->tx.snd_nxt;

    flow_ctx->payload.size = accepted;
    if (rc == TCP_OK) {
        if (!accepted && fin_queued && flow->tx.fin_tx_pending) rc = TCP_WOULDBLOCK;
        else if (accepted || fin_queued) rc = TCP_OK;
        else { 
            if (flow->tx.snd_wnd == 0) {
                if (!flow->timer.persist_active) {
                    flow->timer.persist_active = 1;
                    flow->timer.persist_timer_ms = 0;
                }
                if (flow->timer.persist_timeout_ms == 0) flow->timer.persist_timeout_ms = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
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
        if (flow->tx.nagle_len) flow->tx.nagle_psh = 1;
        if (flow->tx.nagle_len && flow->tx.snd_wnd > 0 && !tcp_flush_nagle(flow, 1)) tcp_daemon_kick();
        tcp_try_send_pending_fin(flow);
        if (flow->tx.fin_tx_pending) tcp_daemon_kick();
        rc = TCP_OK;
    }

    tcp_flow_put(flow);
    return rc;
}