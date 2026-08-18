#include "tcp_internal.h"
#include "networking/transport_layer/socket_bind.h"
#include "networking/transport_layer/csocket_tcp.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/firewall.h"
#include "std/memory.h"
#include "math/rng.h"
#include "random/random.h"
#include "syscalls/syscalls.h"
#include "../tcp.h"

static void tcp_note_dsack(tcp_flow_t *flow, uint32_t left, uint32_t right) {
    if (!flow || !flow->tx.sack_ok || !flow->tx.dsack_enabled || !TCP_SEQ_GT(right, left)) return;
    flow->rx.dsack_pending = 1;
    flow->rx.dsack_left = left;
    flow->rx.dsack_right = right;
}

static bool tcp_reass_count_ok(tcp_flow_t *flow) {
    if (!flow) return false;

    if (flow->rx.reass_count <= TCP_REASS_MAX_SEGS) return true;

    if (flow->rx.reass_count || flow->rx.rcv_ooo_used) tcp_account_ooo_remove(flow->rx.rcv_ooo_used, flow->rx.reass_count);
    flow->rx.reass_count = 0;
    flow->rx.rcv_ooo_used = 0;
    flow->rx.sack_recent_left = 0;
    flow->rx.sack_recent_right = 0;

    for (uint32_t i = 0; i < TCP_REASS_MAX_SEGS; i++) {
        flow->rx.reass[i].seq = 0;
        flow->rx.reass[i].end = 0;
    }

    return false;
}

static void tcp_reass_remove(tcp_flow_t *flow, int32_t idx) {
    if (!tcp_reass_count_ok(flow)) return;
    if (idx < 0 || idx >= flow->rx.reass_count) return;

    uint32_t len = 0;
    if (TCP_SEQ_GT(flow->rx.reass[idx].end, flow->rx.reass[idx].seq)) len = flow->rx.reass[idx].end - flow->rx.reass[idx].seq;

    if (flow->rx.rcv_ooo_used >= len) flow->rx.rcv_ooo_used -= len;
    else flow->rx.rcv_ooo_used = 0;
    tcp_account_ooo_remove(len, 1);

    for (int32_t i = idx; i + 1 < flow->rx.reass_count; i++) flow->rx.reass[i] = flow->rx.reass[i+1];

    if (flow->rx.reass_count) flow->rx.reass_count--;
    flow->rx.reass[flow->rx.reass_count].seq = 0;
    flow->rx.reass[flow->rx.reass_count].end = 0;
}

static bool tcp_reass_drain_inseq(tcp_flow_t *flow) {
    bool advanced = false;
    if (!tcp_reass_count_ok(flow)) return false;

    for(;;){
        int32_t idx = -1;

        for (int32_t i = 0; i < flow->rx.reass_count; i++){
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
        advanced = true;
    }

    if (advanced) tcp_update_adv_wnd(flow, 1);
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
            if (flow->rx.urg_valid && TCP_SEQ_GEQ(flow->rx.rcv_base, flow->rx.urg_seq)) {
                flow->rx.urg_valid = 0;
                flow->rx.urg_seq = 0;
            }
            rc = n;
        }
    }

    if (rc > 0) {
        uint32_t old_edge = flow->rx.rcv_adv_edge;
        uint16_t old_adv_field = flow->base.ctx.window;
        uint32_t old_nxt = flow->rx.rcv_nxt;

        bool advanced = tcp_reass_drain_inseq(flow);
        if (!advanced) tcp_update_adv_wnd(flow, 1);

        if (flow->base.state != TCP_STATE_CLOSED && flow->base.state != TCP_TIME_WAIT) {
            uint32_t threshold = flow->tx.mss ? flow->tx.mss : TCP_DEFAULT_MSS;
            uint32_t half = flow->rx.rcv_wnd_max >> 1;
            if (half && half < threshold) threshold = half;
            if (!threshold) threshold = 1;

            uint32_t delta = TCP_SEQ_GT(flow->rx.rcv_adv_edge, old_edge) ? flow->rx.rcv_adv_edge - old_edge : 0;
            if (advanced || flow->rx.rcv_nxt != old_nxt || (!old_adv_field && flow->base.ctx.window) || delta >= threshold) tcp_send_ack_now(flow);
        }
    }

    tcp_flow_put(flow);
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
    if (!flow) return NULL;
    tcp_tx_seg_t *best = NULL;
    uint32_t best_seq = 0;

    for (uint32_t i = 0; i < TCP_MAX_TX_SEGS; i++){
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

static bool tcp_segment_acceptable(const tcp_flow_t *flow, uint32_t seq, uint32_t seg_len) {
    if (!flow) return false;

    uint32_t rcv_nxt = flow->rx.rcv_nxt;
    uint32_t rcv_wnd = flow->rx.rcv_wnd;
    if (!rcv_wnd) return !seg_len && seq == rcv_nxt;

    uint32_t wnd_end = rcv_nxt + rcv_wnd;
    if (!seg_len) return TCP_SEQ_GEQ(seq, rcv_nxt) && TCP_SEQ_LT(seq, wnd_end);

    uint32_t last = seq + seg_len - 1;
    return (TCP_SEQ_GEQ(seq, rcv_nxt) && TCP_SEQ_LT(seq, wnd_end)) || (TCP_SEQ_GEQ(last, rcv_nxt) && TCP_SEQ_LT(last, wnd_end));
}

static void tcp_send_challenge_ack(tcp_flow_t *flow) {
    if (!flow) return;

    uint32_t now = (uint32_t)get_time();
    if (flow->timer.challenge_ack_valid && now - flow->timer.challenge_ack_last_ms < TCP_CHALLENGE_ACK_INTERVAL_MS) return;
    tcp_send_ack_now(flow);
    if (flow->timer.delayed_ack_pending) return;

    flow->timer.challenge_ack_valid = 1;
    flow->timer.challenge_ack_last_ms = now;
}

static bool tcp_prepare_rcv_buffer(tcp_flow_t *flow) {
    if (!flow || !flow->rx.rcv_wnd_max) return false;
    if (flow->rx.rcv_buf) return true;

    flow->rx.rcv_buf = (uintptr_t)zalloc(flow->rx.rcv_wnd_max);
    if (!flow->rx.rcv_buf) return false;

    flow->rx.rcv_base = flow->rx.rcv_nxt;
    flow->rx.rcv_data_nxt = flow->rx.rcv_nxt;
    tcp_update_adv_wnd(flow, 1);
    return true;
}

static bool tcp_acknowledge_segments(tcp_flow_t *flow, uint32_t ack) {
    if (!flow) return false;

    bool syn_retransmitted = false;
    for (uint32_t i = 0; i < TCP_MAX_TX_SEGS; i++){
        tcp_tx_seg_t *seg = &flow->tx.txq[i];
        if (!seg->used || TCP_SEQ_GEQ(seg->seq, ack)) continue;

        uint32_t end_seq = seg->seq + seg->len + (seg->syn ? 1u : 0u) + (seg->fin ? 1u : 0u);
        if (seg->rtt_sample && TCP_SEQ_GEQ(ack, end_seq)) {
            if (seg->retransmit_cnt == 0) tcp_rtt_update(flow, seg->rtt_timer_ms);
            flow->tx.rtt_sample_pending = 0;
            seg->rtt_sample = 0;
        }

        uint32_t seq = seg->seq;
        if (seg->syn && TCP_SEQ_GT(ack, seq)) {
            if (seg->retransmit_cnt) syn_retransmitted = true;
            seg->syn = 0;
            seq++;
        }

        if (seg->len && TCP_SEQ_GT(ack, seq)) {
            uint32_t n = ack - seq;
            if (n > seg->len) n = (uint32_t)seg->len;
            seg->payload_off += n;
            seg->len -= n;
            seq += n;
            tcp_account_tx_remove(flow, n);
        }
        if (seg->fin && TCP_SEQ_GT(ack, seq)) {
            seg->fin = 0;
            seq++;
        }

        seg->seq = seq;
        if (!seg->syn && !seg->fin && !seg->len) tcp_tx_seg_clear(flow, seg);
    }

    return syn_retransmitted;
}

static void tcp_restart_retransmit_timer(tcp_flow_t *flow) {
    tcp_tx_seg_t *seg = tcp_find_first_unacked(flow);
    if (!seg) return;

    seg->timer_ms = 0;
    seg->timeout_ms = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
}

static bool tcp_sack_insert(tcp_sack_range_t *ranges, uint8_t *count, uint32_t left, uint32_t right) {
    if (!ranges || !count || TCP_SEQ_LEQ(right, left)) return false;

    tcp_sack_range_t merged[TCP_SACK_SCOREBOARD_MAX + 1];
    uint8_t old_count = *count;
    uint8_t out = 0;
    bool inserted = false;

    for (uint8_t i = 0; i < old_count; i++) {
        tcp_sack_range_t current = ranges[i];
        if (TCP_SEQ_LT(current.right, left)) {
            merged[out++] = current;
            continue;
        }

        if (TCP_SEQ_LT(right, current.left)) {
            if (!inserted) {
                merged[out].left = left;
                merged[out].right = right;
                out++;
                inserted = true;
            }
            merged[out++] = current;
            continue;
        }
        
        if (TCP_SEQ_LT(current.left, left)) left = current.left;
        if (TCP_SEQ_GT(current.right, right)) right = current.right;
    }

    if (!inserted) {
        merged[out].left = left;
        merged[out].right = right;
        out++;
    }

    uint8_t first = out > TCP_SACK_SCOREBOARD_MAX ? 1 : 0;
    uint8_t next_count = out - first;
    if (old_count == next_count && memcmp(ranges, &merged[first], sizeof(tcp_sack_range_t)*next_count) == 0) return false;

    memcpy(ranges, &merged[first], sizeof(tcp_sack_range_t) * next_count);
    *count = next_count;
    return true;
}

static void tcp_sack_trim_ranges(tcp_sack_range_t *ranges, uint8_t *count, uint32_t ack) {
    if (!ranges || !count) return;

    uint8_t out = 0;
    for (uint8_t i = 0; i < *count; i++) {
        uint32_t left = ranges[i].left;
        uint32_t right = ranges[i].right;

        if (TCP_SEQ_LEQ(right, ack)) continue;
        if (TCP_SEQ_LT(left, ack)) left = ack;
        ranges[out].left = left;
        ranges[out].right = right;
        out++;
    }

    *count = out;
}

static bool tcp_sack_is_lost(tcp_flow_t *flow, uint32_t right) {
    if (!flow) return false;

    uint32_t high_sacked = flow->tx.sack_range_count ? flow->tx.sack_ranges[flow->tx.sack_range_count - 1].right : flow->tx.snd_una;
    if (TCP_SEQ_GEQ(right, high_sacked)) return false;

    uint32_t bytes = 0;
    uint32_t blocks = 0;
    uint32_t mss = flow->tx.mss ? flow->tx.mss : TCP_DEFAULT_MSS;

    for (uint8_t i = 0; i < flow->tx.sack_range_count; i++) { 
        tcp_sack_range_t *range = &flow->tx.sack_ranges[i];
        if (TCP_SEQ_LEQ(range->right, right)) continue;

        uint32_t left = TCP_SEQ_LT(range->left, right) ? right : range->left;
        if (TCP_SEQ_GT(range->right, left)) bytes += range->right - left;
        blocks++;
    }

    return blocks >= 3 || bytes >= 3 * mss;
}

static bool tcp_sack_select_retransmit(tcp_flow_t *flow, bool force, bool rescue, tcp_tx_seg_t **out_seg, uint32_t *out_left, uint32_t *out_right) {
    if (!flow || !out_seg || !out_left || !out_right) return false;

    tcp_tx_seg_t *best_seg = NULL;
    uint32_t best_left = 0;
    uint32_t best_right = 0;
    uint32_t limit = rescue ? (flow->tx.recover ? flow->tx.recover : flow->tx.snd_nxt) : (flow->tx.sack_range_count ? flow->tx.sack_ranges[flow->tx.sack_range_count-1].right : flow->tx.snd_una);
    uint32_t mss = flow->tx.mss ? flow->tx.mss : TCP_DEFAULT_MSS;

    for (int i = 0; i < TCP_MAX_TX_SEGS; i++) {
        tcp_tx_seg_t *seg = &flow->tx.txq[i];
        if (!seg->used || seg->syn || seg->fin || !seg->len) continue;

        uint32_t start = seg->seq;
        uint32_t end = seg->seq + (uint32_t)seg->len;
        if (TCP_SEQ_LEQ(end, flow->tx.snd_una) || TCP_SEQ_GEQ(start, limit)) continue;
        if (TCP_SEQ_LT(start, flow->tx.snd_una)) start = flow->tx.snd_una;
        if (TCP_SEQ_GT(end, limit)) end = limit;

        uint32_t cursor = start;
        for (uint8_t r = 0; r <= flow->tx.sack_range_count && TCP_SEQ_LT(cursor, end); r++) {
            uint32_t hole_right = end;
            if (r < flow->tx.sack_range_count){
                tcp_sack_range_t *range = &flow->tx.sack_ranges[r];
                if (TCP_SEQ_LEQ(range->right, cursor)) continue;
                if (TCP_SEQ_LEQ(range->left, cursor)) {
                    cursor = range->right;
                    continue;
                }
                if (TCP_SEQ_LT(range->left, hole_right)) hole_right = range->left;
            }
            
            if (TCP_SEQ_GT(hole_right, cursor) && (rescue || force || tcp_sack_is_lost(flow, hole_right))) {
                uint32_t left = cursor;
                uint32_t right = hole_right;

                if (rescue) {
                    if (right - left > mss) left = right - mss;
                    if (!best_seg || TCP_SEQ_GT(right, best_right)) {
                        best_seg = seg;
                        best_left = left;
                        best_right = right;
                    }
                } else {
                    for (uint8_t n = 0; n < flow->tx.sack_retransmitted_count; n++) {
                        tcp_sack_range_t *range = &flow->tx.sack_retransmitted_ranges[n];
                        if (TCP_SEQ_LEQ(range->right, left)) continue;
                        if (TCP_SEQ_GT(range->left, left)) {
                            if (TCP_SEQ_LT(range->left, right)) right = range->left;
                            break;
                        }
                        left = range->right;
                        if (TCP_SEQ_GEQ(left, hole_right)) break;
                    }

                    if (TCP_SEQ_GT(right, left)) {
                        if (right - left > mss) right = left + mss;
                        if (!best_seg || TCP_SEQ_LT(left, best_left)) {
                            best_seg = seg;
                            best_left = left;
                            best_right = right;
                        }
                    }
                }
            }

            if (r < flow->tx.sack_range_count && TCP_SEQ_GT(flow->tx.sack_ranges[r].right, cursor)) cursor = flow->tx.sack_ranges[r].right;
            else break;
        }
    }

    if (!best_seg) return false;
    *out_seg = best_seg;
    *out_left = best_left;
    *out_right = best_right;
    return true;
}

static bool tcp_sack_retransmit_range(tcp_flow_t *flow, tcp_tx_seg_t *seg, uint32_t left, uint32_t right, bool rescue) {
    if (!flow || !seg || TCP_SEQ_LEQ(right, left)) return false;

    tcp_tx_seg_t retransmit = {
        .seq = left,
        .len = right - left,
        .pkt = seg->pkt,
        .payload_off = seg->payload_off + (left - seg->seq),
        .psh = seg->psh && right == seg->seq + seg->len
    };  
    if (!tcp_send_from_seg(flow, &retransmit)) return false;

    for (uint32_t i = 0; i < TCP_MAX_TX_SEGS; i++) flow->tx.txq[i].rtt_sample = 0;
    flow->tx.rtt_sample_pending = 0;
    bool tracked = tcp_sack_insert(flow->tx.sack_retransmitted_ranges, &flow->tx.sack_retransmitted_count, left, right);
    if (rescue) flow->tx.sack_rescue_sent = 1;
    if (seg->retransmit_cnt < UINT8_MAX) seg->retransmit_cnt++;
    seg->timer_ms = 0;
    seg->rtt_timer_ms = 0;
    seg->timeout_ms = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
    return tracked;
}

static void tcp_sack_recovery_send(tcp_flow_t *flow, bool force_first) {
    if (!flow || !flow->tx.sack_ok) return;

    if (force_first) {
        tcp_tx_seg_t *seg = NULL;
        uint32_t left = 0;
        uint32_t right = 0;
        if (!tcp_sack_select_retransmit(flow, true, false, &seg, &left, &right) || !tcp_sack_retransmit_range(flow, seg, left, right, false)) return;
    }

    uint32_t pipe = 0;
    for (int i = 0; i < TCP_MAX_TX_SEGS; i++) {
        tcp_tx_seg_t *seg = &flow->tx.txq[i];

        if (!seg->used) continue;

        uint32_t left = seg->seq;
        uint32_t right = seg->seq + (uint32_t)seg->len + seg->syn + seg->fin;
        if (TCP_SEQ_LEQ(right, flow->tx.snd_una)) continue;
        if (TCP_SEQ_LT(left, flow->tx.snd_una)) left = flow->tx.snd_una;

        uint32_t cursor = left;
        for (uint8_t r = 0; r <= flow->tx.sack_range_count && TCP_SEQ_LT(cursor, right); r++) {
            uint32_t hole_right = right;
            if (r < flow->tx.sack_range_count) {
                tcp_sack_range_t *range = &flow->tx.sack_ranges[r];
                if (TCP_SEQ_LEQ(range->right, cursor)) continue;
                if (TCP_SEQ_LEQ(range->left, cursor)) {
                    cursor = range->right;
                    continue;
                }
                if (TCP_SEQ_LT(range->left, hole_right)) hole_right = range->left;
            }

            if (TCP_SEQ_GT(hole_right, cursor)) {
                if (tcp_sack_is_lost(flow, hole_right)) {
                    uint32_t retransmitted = 0;
                    for (uint8_t n = 0; n < flow->tx.sack_retransmitted_count; n++) {
                        tcp_sack_range_t *range = &flow->tx.sack_retransmitted_ranges[n];
                        if (TCP_SEQ_GEQ(cursor, range->right) || TCP_SEQ_GEQ(range->left, hole_right)) continue;

                        uint32_t range_left = TCP_SEQ_GT(cursor, range->left) ? cursor : range->left;
                        uint32_t range_right = TCP_SEQ_LT(hole_right, range->right) ? hole_right : range->right;
                        if (TCP_SEQ_GT(range_right, range_left)) retransmitted += range_right - range_left;
                    }

                    uint32_t hole_len = hole_right - cursor;
                    pipe += retransmitted > hole_len ? hole_len : retransmitted;
                } else {
                    pipe += hole_right - cursor;
                }
            }

            if (r < flow->tx.sack_range_count && TCP_SEQ_GT(flow->tx.sack_ranges[r].right, cursor)) cursor = flow->tx.sack_ranges[r].right;
            else break;
        }
    }

    uint32_t cwnd = flow->tx.cwnd ? flow->tx.cwnd : (flow->tx.mss ? flow->tx.mss : TCP_DEFAULT_MSS);

    for (uint32_t sent = 0; sent < TCP_SACK_SCOREBOARD_MAX && pipe < cwnd; sent++) {
        tcp_tx_seg_t *seg = NULL;
        uint32_t left = 0;
        uint32_t right = 0;
        bool rescue = false;
        if (!tcp_sack_select_retransmit(flow, false, false, &seg, &left, &right)) {
            if (flow->tx.sack_rescue_sent || !tcp_sack_select_retransmit(flow, false, true, &seg, &left, &right)) break;
            rescue = true;
        }
        uint32_t len = right - left;
        if (!len || len > cwnd - pipe) break;
        if (!tcp_sack_retransmit_range(flow, seg, left, right, rescue)) break;
        pipe += len;
    }
}

static bool tcp_apply_sack_blocks(tcp_flow_t *flow, const tcp_parsed_opts_t *opts) {
    if (!flow || !opts || !flow->tx.sack_ok || !opts->sack_count) return false;

    bool changed = false;
    for (uint32_t b = 0; b < opts->sack_count; b++) {
        uint32_t left = opts->sacks[b].left;
        uint32_t right = opts->sacks[b].right;
        if (TCP_SEQ_LEQ(right, left)) continue;
        if (TCP_SEQ_LEQ(right, flow->tx.snd_una)) continue;
        if (TCP_SEQ_GEQ(left, flow->tx.snd_nxt)) continue;
        if (TCP_SEQ_LT(left, flow->tx.snd_una)) left = flow->tx.snd_una;
        if (TCP_SEQ_GT(right, flow->tx.snd_nxt)) right = flow->tx.snd_nxt;
        if (TCP_SEQ_LEQ(right, left)) continue;
        changed |= tcp_sack_insert(flow->tx.sack_ranges, &flow->tx.sack_range_count, left, right);
    }

    return changed;
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
    f->tx.recover = f->tx.snd_nxt;
    f->tx.recover_valid = 1;

    f->tx.sack_range_count = 0;
    f->tx.sack_retransmitted_count = 0;
    f->tx.sack_rescue_sent = 0;
}

static void tcp_cc_on_new_ack(tcp_flow_t *f, uint32_t ack, uint32_t prev_una) {
    uint32_t mss = f->tx.mss ? f->tx.mss : TCP_DEFAULT_MSS;
    uint32_t acked = ack - prev_una;

    if (f->tx.in_fast_recovery){
        if (TCP_SEQ_GEQ(ack, f->tx.recover)){
            f->tx.cwnd = f->tx.ssthresh;
            if (f->tx.cwnd < mss) f->tx.cwnd = mss;

            f->tx.in_fast_recovery = 0;
            f->tx.recover_valid = 0;
            f->tx.dup_acks = 0;
            f->tx.cwnd_acc = 0;
            f->tx.sack_range_count = 0;
            f->tx.sack_retransmitted_count = 0;
            f->tx.sack_rescue_sent = 0;
            return;
        }

        if (f->tx.sack_ok) {
            f->tx.cwnd = f->tx.ssthresh;
            if (f->tx.cwnd < mss) f->tx.cwnd = mss;
            return;
        }

        uint32_t cwnd = acked < f->tx.cwnd ? f->tx.cwnd - acked : 0;
        if (acked >= mss) {
            uint32_t room = UINT32_MAX - cwnd;
            cwnd += room < mss ? room : mss;
        }
        if (cwnd < mss) cwnd = mss;
        f->tx.cwnd = cwnd;

        tcp_tx_seg_t *seg = tcp_find_first_unacked(f);
        if (seg && !tcp_retransmit_seg(f, seg)) tcp_restart_retransmit_timer(f);
        return;
    }

    if (f->tx.recover_valid && TCP_SEQ_GEQ(ack, f->tx.recover)) f->tx.recover_valid = 0;

    if (f->tx.cwnd < f->tx.ssthresh){
        uint32_t inc = acked < mss ? acked : mss;
        f->tx.cwnd += inc;
        if (f->tx.cwnd < mss) f->tx.cwnd = mss;
        return;
    }

    uint64_t acc = (uint64_t) f->tx.cwnd_acc + acked;
    if (acc >= f->tx.cwnd) {
        acc -= f->tx.cwnd;
        f->tx.cwnd += mss;
    }
    f->tx.cwnd_acc = acc > UINT32_MAX ? UINT32_MAX : (uint32_t)acc;
}

static void tcp_cc_on_dupack(tcp_flow_t *f) {
    uint32_t mss = f->tx.mss ? f->tx.mss : TCP_DEFAULT_MSS;

    if (f->tx.in_fast_recovery){
        if (f->tx.sack_ok) {
            tcp_sack_recovery_send(f, false);
            return;
        }
        uint32_t room = UINT32_MAX - f->tx.cwnd;
        f->tx.cwnd += room < mss ? room : mss;
        return;
    }

    if (f->tx.dup_acks != 3) return;
    if (f->tx.recover_valid && TCP_SEQ_LEQ(f->tx.snd_una, f->tx.recover)) return;

    uint32_t flight = f->tx.snd_nxt - f->tx.snd_una;
    uint32_t half = flight / 2;
    uint32_t minth = 2u * mss;

    if (half < minth) half = minth;

    f->tx.ssthresh = half;
    f->tx.recover = f->tx.snd_nxt;
    f->tx.recover_valid = 1;
    if (f->tx.sack_ok) f->tx.cwnd = f->tx.ssthresh;
    else {
        uint64_t recovery_cwnd = (uint64_t)f->tx.ssthresh + 3u * mss;
        f->tx.cwnd = recovery_cwnd > UINT32_MAX ? UINT32_MAX : (uint32_t)recovery_cwnd;
    }
    f->tx.in_fast_recovery = 1;
    f->tx.sack_retransmitted_count = 0;
    f->tx.sack_rescue_sent = 0;

    if (f->tx.sack_ok) tcp_sack_recovery_send(f, true);
    else {
        tcp_tx_seg_t *seg = tcp_find_first_unacked(f);
        if (seg && !tcp_retransmit_seg(f, seg)) tcp_restart_retransmit_timer(f);
    }
}

void tcp_input(ip_version_t ipver, const void *src_ip_addr, const void *dst_ip_addr, uint8_t l3_id, netpkt_t* pkt) {
    if (!pkt) return;
    if (!src_ip_addr || !dst_ip_addr || (ipver != IP_VER4 && ipver != IP_VER6)) {
        netpkt_unref(pkt);
        return;
    }
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
    uint16_t urgent_ptr = bswap16(hdr.urgent_ptr);

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

    tcp_flow_t *flow = tcp_flow_acquire_match(dst_port, ipver, dst_ip_addr, src_ip_addr, src_port);

    uint8_t ifx = 0;
    if (ipver == IP_VER4) {
        l3_ipv4_interface_t *v4 = l3_ipv4_find_by_id(l3_id);
        if (!v4 || !v4->l2) {
            if (flow) tcp_flow_put(flow);
            netpkt_unref(pkt);
            return;
        }
        uint32_t src_ip = 0;
        uint32_t dst_ip = 0;
        memcpy(&src_ip, src_ip_addr, sizeof(src_ip));
        memcpy(&dst_ip, dst_ip_addr, sizeof(dst_ip));
        bool dst_invalid = ipv4_is_unspecified(dst_ip) || ipv4_is_multicast(dst_ip) || ipv4_is_limited_broadcast(dst_ip) || ipv4_is_directed_broadcast(v4->ip, v4->mask, dst_ip);
        bool src_invalid = ipv4_is_unspecified(src_ip) || ipv4_is_multicast(src_ip) || ipv4_is_limited_broadcast(src_ip) || ipv4_is_directed_broadcast(v4->ip, v4->mask, src_ip);
        if (src_invalid || dst_invalid) {
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
        if (ipv6_is_unspecified((const uint8_t*)dst_ip_addr) || ipv6_is_multicast((const uint8_t*)dst_ip_addr) || ipv6_is_unspecified((const uint8_t*)src_ip_addr) || ipv6_is_multicast((const uint8_t*)src_ip_addr)) {
            if (flow) tcp_flow_put(flow);
            netpkt_unref(pkt);
            return;
        }
        ifx = v6->l2->ifindex;
    }

    if (flow) {
        if (flow->base.l3_id != l3_id) {
            flow->base.l3_id = l3_id;
            flow->tx.path_mss = tcp_calc_mss_for_l3(l3_id, ipver, src_ip_addr);
            tcp_update_mss(flow);
        }
    }

    if (flow) {
        switch (flow->base.state) {
            case TCP_TIME_WAIT:
                if ((flags & (1 << SYN_F)) && !(flags& ((1 << ACK_F) | (1 << RST_F) | (1 << FIN_F))) && data_len == 0) {
                    ksocket_t* listener = socket_bind_lookup(PROTO_TCP, ipver, l3_id, ifx, src_ip_addr, src_port, dst_ip_addr, dst_port);
                    if (listener) {
                        socket_core_put(listener);
                        tcp_free_flow(flow);
                        tcp_flow_put(flow);
                        flow = NULL;
                    }
                }

                break;

            case TCP_SYN_RECEIVED:
                if ((flags & (1 << SYN_F)) && !(flags& ((1 << RST_F) | (1 << FIN_F))) && data_len == 0 && seq+1 == flow->rx.rcv_nxt && (!(flags & (1 << ACK_F)) || (flow->base.active_open && TCP_SEQ_GT(ack, flow->tx.snd_una) && TCP_SEQ_LEQ(ack, flow->tx.snd_nxt)))) {
                    if ((flags & (1 << ACK_F)) && flow->base.active_open && TCP_SEQ_GT(ack, flow->tx.snd_una) && TCP_SEQ_LEQ(ack, flow->tx.snd_nxt)) {
                        if (!tcp_prepare_rcv_buffer(flow)) {
                            tcp_send_reset(l3_id, ipver, dst_ip_addr, src_ip_addr, dst_port, src_port, ack, 0, false);
                            tcp_free_flow(flow);
                            tcp_flow_put(flow);
                            netpkt_unref(pkt);
                            return;
                        }

                        bool syn_retransmitted = tcp_acknowledge_segments(flow, ack);
                        if (syn_retransmitted && !flow->tx.rtt_valid && flow->tx.rto < TCP_SYN_DATA_RTO) flow->tx.rto = TCP_SYN_DATA_RTO;
                        flow->tx.snd_una = ack;
                        flow->tx.snd_wnd = window;
                        flow->tx.snd_wl1 = seq;
                        flow->tx.snd_wl2 = ack;
                        flow->base.ctx.ack_received = ack;
                        flow->base.ctx.sequence = flow->tx.snd_nxt;
                        flow->base.ctx.flags = 0;
                        flow->base.state = TCP_ESTABLISHED;
                        flow->base.active_open = 0;
                        flow->timer.keepalive_idle_ms = 0;
                        flow->timer.delayed_ack_pending = 0;
                        flow->timer.delayed_ack_timer_ms = 0;
                        tcp_restart_retransmit_timer(flow);
                        tcp_send_ack_now(flow);
                    } else if (!(flags & (1u << ACK_F))) {
                        for (uint32_t i = 0; i < TCP_MAX_TX_SEGS; i++) {
                            tcp_tx_seg_t *seg = &flow->tx.txq[i];
                            if (!seg->used || !seg->syn) continue;
                            if (!tcp_retransmit_seg(flow, seg)) tcp_restart_retransmit_timer(flow);
                            break;
                        }
                    }
                    tcp_flow_put(flow);
                    netpkt_unref(pkt);
                    return;
                }
                break;

            default:
                break;
        }
    }

    if (!flow){
        net_l4_endpoint remote = {0};
        make_ep(src_ip_addr, src_port, ipver, &remote);
        if (!firewall_allows(PROTO_TCP, NET_CTRL_FIREWALL_IN, &remote, dst_port, false)) {
            netpkt_unref(pkt);
            return;
        }

        bool initial_syn = (flags & (1u << SYN_F)) && !(flags & ((1u << ACK_F) | (1u << RST_F) | (1u << FIN_F))) && data_len == 0;
        ksocket_t* listener = socket_bind_lookup(PROTO_TCP, ipver, l3_id, ifx, src_ip_addr, src_port, dst_ip_addr, dst_port);

        if (initial_syn && listener){
            rng_t rng;
            rng_init_random(&rng);

            tcp_admit_result_t syn_admit = tcp_admit_syn(listener, ipver, src_ip_addr);
            if (syn_admit != TCP_ADMIT_OK) {
                if (syn_admit == TCP_ADMIT_SYN_GLOBAL) tcp_stats.syn_drop_global++;
                else if (syn_admit == TCP_ADMIT_SYN_LISTENER) tcp_stats.syn_drop_listener++;
                else if (syn_admit == TCP_ADMIT_SYN_SOURCE) tcp_stats.syn_drop_source++;
                else if (syn_admit == TCP_ADMIT_FLOW_RESERVE) tcp_stats.syn_drop_flow_reserve++;
                else if (syn_admit == TCP_ADMIT_FLOW_TABLE_FULL) tcp_stats.flow_table_full++;
                socket_core_put(listener);
                netpkt_unref(pkt);
                return;
            }

            const SocketOptions* listener_extra = socket_tcp_options(socket_core_impl(listener));
            tcp_flow_t *nf = tcp_alloc_flow();
            if (!nf) {
                socket_core_put(listener);
                netpkt_unref(pkt);
                return;
            }
            flow = nf;
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

            flow->tx.path_mss = tcp_calc_mss_for_l3(l3_id, ipver, src_ip_addr);
            flow->tx.peer_mss = pop.has_mss && pop.mss ? pop.mss : (ipver == IP_VER6 ? TCP_DEFAULT_PEER_MSS_IPV6 : TCP_DEFAULT_PEER_MSS_IPV4);
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
            flow->tx.snd_wnd = window;
            flow->tx.snd_wl1 = seq;
            flow->tx.snd_wl2 = 0;

            flow->timer.persist_active = 0;
            flow->timer.persist_timer_ms = 0;
            flow->timer.persist_timeout_ms = 0;

            flow->timer.delayed_ack_pending = 0;
            flow->timer.delayed_ack_timer_ms = 0;

            uint32_t rcvbuf = TCP_DEFAULT_RCV_BUF;
            if (listener_extra && (listener_extra->flags & SOCK_OPT_BUF_SIZE) && listener_extra->buf_size) rcvbuf = listener_extra->buf_size;
            flow->rx.rcv_wnd_max = tcp_clamp_rcvbuf(rcvbuf);
            tcp_flow_apply_options(flow, listener_extra, UINT32_MAX);
            flow->tx.sack_ok = flow->tx.sack_enabled && pop.sack_permitted;
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
            tcp_update_adv_wnd(flow, flow->tx.ws_ok ? 1 : 0);

            flow->tx.cwnd = tcp_initial_cwnd(flow->tx.mss);
            flow->tx.ssthresh = TCP_RECV_WINDOW;
            flow->tx.dup_acks = 0;
            flow->tx.in_fast_recovery = 0;
            flow->tx.recover = 0;
            flow->tx.recover_valid = 0;
            flow->tx.cwnd_acc = 0;

            flow->timer.time_wait_ms = 0;
            flow->timer.fin_wait2_ms = 0;
            flow->base.listener = listener;
            listener = NULL;

            tcp_tx_seg_t *seg = tcp_alloc_tx_seg(flow, 0);
            if (!seg) {
                tcp_free_flow(flow);
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }

            seg->syn = 1;
            seg->fin = 0;
            seg->psh = 0;
            seg->persist = 0;
            seg->rtt_sample = 1;
            flow->tx.rtt_sample_pending = 1;
            seg->retransmit_cnt = 0;
            seg->seq = iss;
            seg->len = 0;
            seg->pkt = NULL;
            seg->payload_off = 0;
            seg->timer_ms = 0;
            seg->timeout_ms = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
            seg->opts_len = tcp_build_syn_options(seg->opts, (uint16_t)flow->tx.advertised_mss, flow->tx.ws_ok ? flow->tx.ws_send : 0xff, flow->tx.sack_ok);
            flow->tx.snd_nxt = iss + 1;
            flow->base.ctx.sequence = flow->tx.snd_nxt;
            if (!tcp_active_insert_flow(flow) || !tcp_send_from_seg(flow, seg)) {
                tcp_free_flow(flow);
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }
            tcp_flow_put(flow);
            netpkt_unref(pkt);
            return;
        }

        if (listener) socket_core_put(listener);
        if (!(flags & (1u << RST_F))){
            if (flags & (1u << ACK_F)){
                tcp_send_reset(l3_id, ipver, dst_ip_addr, src_ip_addr, dst_port, src_port, ack, 0, false);
            } else {
                uint32_t seg_len = data_len;

                if (flags & (1u << SYN_F)) seg_len++;
                if (flags & (1u << FIN_F)) seg_len++;

                tcp_send_reset(l3_id, ipver, dst_ip_addr, src_ip_addr, dst_port, src_port, seq, seq + seg_len, true);
            }
        }

        netpkt_unref(pkt);
        return;
    }

    uint8_t fin = (flags & (1 << FIN_F)) ? 1 : 0;
    uint32_t seg_seq = seq;
    bool handshake_ack_processed = false;

    switch (flow->base.state) {
        case TCP_TIME_WAIT:
            if (flags & (1u << RST_F)) {
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }

            uint32_t time_wait_seg_len = data_len;

            if (flags & (1u << SYN_F)) time_wait_seg_len++;
            if (flags & (1u << FIN_F)) time_wait_seg_len++;

            uint32_t seg_end = seq + time_wait_seg_len;

            if (TCP_SEQ_LEQ(seq, flow->rx.rcv_nxt) && TCP_SEQ_GEQ(seg_end, flow->rx.rcv_nxt)){
                flow->timer.time_wait_ms = 0;
                tcp_send_ack_now(flow);
            }

            tcp_flow_put(flow);
            netpkt_unref(pkt);
            return;

        case TCP_SYN_SENT:
            bool ack_set = (flags & (1u << ACK_F)) != 0;
            bool ack_acceptable = false;

            if (ack_set) {
                if (TCP_SEQ_LEQ(ack, flow->tx.snd_una) || TCP_SEQ_GT(ack, flow->tx.snd_nxt)) {
                    if (!(flags & (1u << RST_F))) tcp_send_reset(l3_id, ipver, dst_ip_addr, src_ip_addr, dst_port, src_port, ack, 0, false);
                    tcp_flow_put(flow);
                    netpkt_unref(pkt);
                    return;
                }
                ack_acceptable = true;
            }

            if (flags & (1u << RST_F)) {
                if (ack_acceptable) tcp_free_flow(flow);
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }

            if (!(flags & (1u << SYN_F))) {
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }

            flow->timer.keepalive_idle_ms = 0;
            flow->base.ctx.ack = seq + 1;
            flow->rx.rcv_nxt = seq + 1;
            flow->rx.rcv_base = flow->rx.rcv_nxt;
            flow->rx.rcv_data_nxt = flow->rx.rcv_nxt;
            flow->rx.rcv_ooo_used = 0;
            flow->rx.sack_recent_left = 0;
            flow->rx.sack_recent_right = 0;

            tcp_parsed_opts_t pop = parsed_opts;

            flow->tx.ws_recv = pop.has_wscale ? pop.wscale : 0;
            if (flow->tx.ws_recv > 14) flow->tx.ws_recv = 14;
            flow->tx.ws_ok = (flow->tx.ws_send != 0) && pop.has_wscale ? 1 : 0;
            if (!flow->tx.ws_ok) {
                flow->tx.ws_send = 0;
                flow->tx.ws_recv = 0;
            }

            flow->tx.sack_ok = flow->tx.sack_enabled && pop.sack_permitted;

            flow->tx.path_mss = tcp_calc_mss_for_l3(l3_id, ipver, src_ip_addr);
            flow->tx.peer_mss = pop.has_mss && pop.mss ? pop.mss : (ipver == IP_VER6 ? TCP_DEFAULT_PEER_MSS_IPV6 : TCP_DEFAULT_PEER_MSS_IPV4);
            tcp_update_mss(flow);
            flow->tx.cwnd = tcp_initial_cwnd(flow->tx.mss);
            flow->tx.snd_wnd = window;
            flow->tx.snd_wl1 = seq;
            flow->tx.snd_wl2 = ack_set ? ack : 0;
            tcp_update_adv_wnd(flow, 1);

            if (!ack_acceptable) {
                flow->base.state = TCP_SYN_RECEIVED;
                for (uint32_t i = 0; i < TCP_MAX_TX_SEGS; i++) {
                    tcp_tx_seg_t *seg = &flow->tx.txq[i];
                    if (!seg->used || !seg->syn) continue;
                    if (!tcp_retransmit_seg(flow, seg)) tcp_restart_retransmit_timer(flow);
                    break;
                }
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }

            if (!tcp_prepare_rcv_buffer(flow)) {
                tcp_send_reset(l3_id, ipver, dst_ip_addr, src_ip_addr, dst_port, src_port, ack, 0, false);
                tcp_free_flow(flow);
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }
            bool syn_retransmitted = tcp_acknowledge_segments(flow, ack);
            if (syn_retransmitted && !flow->tx.rtt_valid && flow->tx.rto < TCP_SYN_DATA_RTO) flow->tx.rto = TCP_SYN_DATA_RTO;
            flow->tx.snd_una = ack;
            flow->base.ctx.ack_received = ack;
            flow->base.ctx.sequence = flow->tx.snd_nxt;
            flow->base.ctx.flags = 0;
            flow->base.state = TCP_ESTABLISHED;
            flow->base.active_open = 0;
            flow->timer.delayed_ack_pending = 0;
            flow->timer.delayed_ack_timer_ms = 0;
            tcp_restart_retransmit_timer(flow);
            tcp_send_ack_now(flow);

            if (!data_len && !fin) {
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }

            seg_seq = seq + 1;
            if (!tcp_segment_acceptable(flow, seg_seq, data_len + fin)) {
                tcp_send_ack_now(flow);
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }

            handshake_ack_processed = true;
            break;


        case TCP_SYN_RECEIVED:
            uint32_t syn_seg_len = data_len + ((flags & (1u << SYN_F)) ? 1 : 0) + fin;
            bool seq_acceptable = tcp_segment_acceptable(flow, seq, syn_seg_len);

            if (flags & (1u << RST_F)) {
                if (seq == flow->rx.rcv_nxt) tcp_free_flow(flow);
                else if (tcp_segment_acceptable(flow, seq, 0)) tcp_send_challenge_ack(flow);
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }

            if (flags & (1u << SYN_F)) {
                tcp_send_challenge_ack(flow);
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }

            if (!seq_acceptable) {
                tcp_send_ack_now(flow);
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }

            if (!(flags & (1u << ACK_F))) {
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }

            if (TCP_SEQ_LEQ(ack, flow->tx.snd_una) || TCP_SEQ_GT(ack, flow->tx.snd_nxt)) {
                tcp_send_reset(l3_id, ipver, dst_ip_addr, src_ip_addr, dst_port, src_port, ack, 0, false);
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }

            flow->timer.keepalive_idle_ms = 0;

            if (!tcp_prepare_rcv_buffer(flow)) {
                tcp_send_reset(l3_id, ipver, dst_ip_addr, src_ip_addr, dst_port, src_port, 0, flow->base.ctx.ack, true);
                tcp_free_flow(flow);
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }

            if (!flow->base.active_open) {
                uint32_t queued = 0;
                if (flow->base.listener) queued = tcp_accept_enqueue(flow->base.listener, ipver, src_ip_addr, dst_ip_addr, src_port, dst_port);
                if (!queued) {
                    tcp_stats.acceptq_drop_full++;
                    tcp_hdr_t rst_hdr;
                    rst_hdr.src_port = bswap16(flow->base.local.port);
                    rst_hdr.dst_port = bswap16(flow->base.remote.port);
                    rst_hdr.sequence = bswap32(flow->tx.snd_nxt);
                    rst_hdr.ack = bswap32(flow->base.ctx.ack);
                    rst_hdr.flags = (uint8_t)((1 << RST_F) | (1 << ACK_F));
                    rst_hdr.window = 0;
                    rst_hdr.urgent_ptr = 0;

                    (void)tcp_send_flow_segment(flow, &rst_hdr, NULL, 0, NULL, 0);

                    tcp_free_flow(flow);
                    tcp_flow_put(flow);
                    netpkt_unref(pkt);
                    return;
                }

                if (flow->base.listener) {
                    socket_core_put(flow->base.listener);
                    flow->base.listener = NULL;
                }
            }

            bool syn_retransmitted_recv = tcp_acknowledge_segments(flow, ack);
            if (syn_retransmitted_recv && !flow->tx.rtt_valid && flow->tx.rto < TCP_SYN_DATA_RTO) flow->tx.rto = TCP_SYN_DATA_RTO;
            flow->tx.snd_una = ack;
            flow->base.ctx.sequence = flow->tx.snd_nxt;
            flow->base.ctx.ack_received = ack;
            flow->base.ctx.flags = 0;
            flow->base.state = TCP_ESTABLISHED;
            flow->base.active_open = 0;
            flow->timer.delayed_ack_pending = 0;
            flow->timer.delayed_ack_timer_ms = 0;

            uint32_t new_wnd = window;
            if (flow->tx.ws_ok && flow->tx.ws_recv) new_wnd = new_wnd << flow->tx.ws_recv;
            flow->tx.snd_wnd = new_wnd;
            flow->tx.snd_wl1 = seq;
            flow->tx.snd_wl2 = ack;
            tcp_restart_retransmit_timer(flow);

            if (!data_len && !fin) {
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }

            handshake_ack_processed = true;
            break;


    default:
        break;
    }

    if (!handshake_ack_processed) {
        uint32_t seg_len = data_len + ((flags & (1u << SYN_F)) ? 1u : 0u) + fin;

        if (flags & (1u << RST_F)){
            if (seq == flow->rx.rcv_nxt) tcp_free_flow(flow);
            else if (tcp_segment_acceptable(flow, seq, 0)) tcp_send_challenge_ack(flow);
            tcp_flow_put(flow);
            netpkt_unref(pkt);
            return;
        }

        if (flags & (1u << SYN_F)) {
            tcp_send_challenge_ack(flow);
            tcp_flow_put(flow);
            netpkt_unref(pkt);
            return;
        }

        if (!tcp_segment_acceptable(flow, seq, seg_len)) {
            if (data_len && TCP_SEQ_LT(seq, flow->rx.rcv_nxt)) {
                uint32_t duplicate_right = seq + data_len;
                if (TCP_SEQ_GT(duplicate_right, flow->rx.rcv_nxt)) duplicate_right = flow->rx.rcv_nxt;
                tcp_note_dsack(flow, seq, duplicate_right);
            }
            tcp_send_ack_now(flow);
            tcp_flow_put(flow);
            netpkt_unref(pkt);
            return;
        }

        if (!(flags & (1u << ACK_F))) {
            tcp_flow_put(flow);
            netpkt_unref(pkt);
            return;
        }

        if (TCP_SEQ_GT(ack, flow->tx.snd_nxt)) {
            tcp_send_ack_now(flow);
            tcp_flow_put(flow);
            netpkt_unref(pkt);
            return;
        }

        flow->timer.keepalive_idle_ms = 0;
        uint32_t old_wnd = flow->tx.snd_wnd;
        uint32_t new_wnd = window;
        if (flow->tx.ws_ok && flow->tx.ws_recv) new_wnd <<= flow->tx.ws_recv;
        if (TCP_SEQ_GEQ(ack, flow->tx.snd_una) && (TCP_SEQ_LT(flow->tx.snd_wl1, seq) || (flow->tx.snd_wl1 == seq && TCP_SEQ_LEQ(flow->tx.snd_wl2, ack)))) {
            flow->tx.snd_wnd = new_wnd;
            flow->tx.snd_wl1 = seq;
            flow->tx.snd_wl2 = ack;
            if (new_wnd) {
                flow->timer.persist_active = 0;
                flow->timer.persist_timer_ms = 0;
                flow->timer.persist_timeout_ms = 0;
            }
        }

        if (TCP_SEQ_GT(ack, flow->tx.snd_una)) {
            uint32_t prev_una = flow->tx.snd_una;

            flow->tx.snd_una = ack;
            flow->base.ctx.ack_received = ack;
            flow->tx.dup_acks = 0;
            bool syn_retransmitted = tcp_acknowledge_segments(flow, ack);
            if (syn_retransmitted && !flow->tx.rtt_valid && flow->tx.rto < TCP_SYN_DATA_RTO) flow->tx.rto = TCP_SYN_DATA_RTO;

            tcp_sack_trim_ranges(flow->tx.sack_ranges, &flow->tx.sack_range_count, ack);
            tcp_sack_trim_ranges(flow->tx.sack_retransmitted_ranges, &flow->tx.sack_retransmitted_count, ack);
            if (flow->tx.sack_ok && parsed_opts.sack_count) (void)tcp_apply_sack_blocks(flow, &parsed_opts);
            tcp_cc_on_new_ack(flow, ack, prev_una);
            tcp_restart_retransmit_timer(flow);
            if (flow->tx.in_fast_recovery && flow->tx.sack_ok) tcp_sack_recovery_send(flow, false);

            if (flow->base.state == TCP_FIN_WAIT_1 && TCP_SEQ_GEQ(ack, flow->base.ctx.expected_ack)) {
                flow->base.state = TCP_FIN_WAIT_2;
                flow->timer.fin_wait2_ms = 0;
                tcp_daemon_kick();
            } else if (flow->base.state == TCP_CLOSING && TCP_SEQ_GEQ(ack, flow->base.ctx.expected_ack)) {
                tcp_enter_time_wait(flow);
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            } else if (flow->base.state == TCP_LAST_ACK && TCP_SEQ_GEQ(ack, flow->base.ctx.expected_ack)) {
                tcp_free_flow(flow);
                tcp_flow_put(flow);
                netpkt_unref(pkt);
                return;
            }
        } else if (ack == flow->tx.snd_una && data_len == 0 && !fin) {
            bool sack_changed = flow->tx.sack_ok && parsed_opts.sack_count && tcp_apply_sack_blocks(flow, &parsed_opts);
            if (old_wnd && flow->tx.snd_wnd && TCP_SEQ_LT(flow->tx.snd_una, flow->tx.snd_nxt) && (flow->tx.snd_wnd == old_wnd || sack_changed)) {
                if (flow->tx.dup_acks < UINT8_MAX) flow->tx.dup_acks++;
                tcp_cc_on_dupack(flow);
            }
        } else flow->tx.dup_acks = 0;
    }

    if (flags & (1 << URG_F)) {
        //do we really need this in 2026?
        uint32_t urg_seq = seq + urgent_ptr;
        if (TCP_SEQ_GT(urg_seq, flow->rx.rcv_base) && (!flow->rx.urg_valid || TCP_SEQ_GT(urg_seq, flow->rx.urg_seq))) {
            flow->rx.urg_valid = 1;
            flow->rx.urg_seq = urg_seq;
        }
    }

    if (flow->tx.snd_wnd > 0) {
        tcp_tx_seg_t *best = tcp_find_first_unacked(flow);
        if (best && best->persist) {
            best->persist = 0;
            if (!tcp_retransmit_seg(flow, best)) tcp_restart_retransmit_timer(flow);
        }
    }

    if (flow->tx.nagle_len && flow->tx.snd_wnd > 0 && !tcp_flush_nagle(flow, flow->tx.fin_tx_pending ? 1 : 0)) tcp_daemon_kick();
    if (flow->tx.fin_tx_pending) {
        tcp_try_send_pending_fin(flow);
        if (flow->tx.fin_tx_pending) tcp_daemon_kick();
    }

    bool need_ack = false;
    bool ack_immediate = false;

    if (data_len || fin) {
        uint32_t rcv_nxt = flow->rx.rcv_nxt;
        uint32_t wnd_end = rcv_nxt + flow->rx.rcv_wnd;

        uint32_t orig_data_len = data_len;
        uint8_t fin_in = fin;
        uint8_t had_reass = flow->rx.reass_count ? 1 : 0;
        uint32_t fin_seq = seg_seq + orig_data_len;
        uint32_t orig_end = seg_seq + orig_data_len + (fin ? 1u : 0u);
        bool discard_payload = flow->base.state == TCP_FIN_WAIT_1 || flow->base.state == TCP_FIN_WAIT_2 || flow->base.state == TCP_CLOSING || flow->base.state == TCP_LAST_ACK || flow->base.state == TCP_TIME_WAIT;

        if (orig_data_len && TCP_SEQ_LT(seg_seq, rcv_nxt)) {
            uint32_t duplicate_right = seg_seq + orig_data_len;
            if (TCP_SEQ_GT(duplicate_right, rcv_nxt)) duplicate_right = rcv_nxt;
            tcp_note_dsack(flow, seg_seq, duplicate_right);
        }

        if (TCP_SEQ_LEQ(orig_end, rcv_nxt) || TCP_SEQ_GEQ(seg_seq, wnd_end)) {
            need_ack = true;
            ack_immediate = true;
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
                need_ack = true;
                ack_immediate = true;
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
                    if (offer && discard_payload) accepted = offer;
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
                        tcp_update_adv_wnd(flow, 1);
                        ack_immediate = true;
                    }

                    if (accepted < data_len) ack_immediate = true;
                }

                if (fin_in) {
                    if (flow->rx.rcv_nxt == fin_seq) {
                        flow->rx.rcv_nxt += 1;
                        flow->base.ctx.ack = flow->rx.rcv_nxt;

                        tcp_state_t old = flow->base.state;

                        if (old == TCP_ESTABLISHED) flow->base.state = TCP_CLOSE_WAIT;
                        else if (old == TCP_FIN_WAIT_1) flow->base.state = TCP_CLOSING;
                        else if (old == TCP_FIN_WAIT_2) tcp_enter_time_wait(flow);

                        ack_immediate = true;
                    } else {
                        flow->rx.fin_pending = 1;
                        flow->rx.fin_seq = fin_seq;
                    }
                }

                if (tcp_reass_drain_inseq(flow)) ack_immediate = true;
                if (had_reass) ack_immediate = true;

                if (flow->rx.fin_pending && flow->rx.fin_seq == flow->rx.rcv_nxt){
                    flow->rx.fin_pending = 0;
                    flow->rx.rcv_nxt += 1;
                    flow->base.ctx.ack = flow->rx.rcv_nxt;

                    tcp_state_t old = flow->base.state;

                    if (old == TCP_ESTABLISHED) flow->base.state = TCP_CLOSE_WAIT;
                    else if (old == TCP_FIN_WAIT_1) flow->base.state = TCP_CLOSING;
                    else if (old == TCP_FIN_WAIT_2) tcp_enter_time_wait(flow);

                    ack_immediate = true;
                }

                tcp_update_adv_wnd(flow, 1);

                need_ack = true;
            } else {
                if (!discard_payload && data_len && flow->rx.rcv_buf && flow->rx.rcv_wnd_max) {
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
                        uint8_t overlapping = 0;
                        bool covered = false;

                        for (uint32_t i = 0; ooo_len && i < flow->rx.reass_count; i++) {
                            tcp_reass_seg_t *r = &flow->rx.reass[i];
                            if (TCP_SEQ_LEQ(r->seq, start) && TCP_SEQ_GEQ(r->end, end)) {
                                flow->rx.sack_recent_left = r->seq;
                                flow->rx.sack_recent_right = r->end;
                                tcp_note_dsack(flow, start, end);
                                covered = true;
                                break;
                            }

                            if (TCP_SEQ_LT(r->end, merged_start) || TCP_SEQ_GT(r->seq, merged_end)) continue;
                            if (flow->tx.sack_ok && flow->tx.dsack_enabled && !flow->rx.dsack_pending) {
                                uint32_t duplicate_left = TCP_SEQ_GT(start, r->seq) ? start : r->seq;
                                uint32_t duplicate_right = TCP_SEQ_LT(end, r->end) ? end : r->end;
                                tcp_note_dsack(flow, duplicate_left, duplicate_right);
                            }

                            if (TCP_SEQ_LT(r->seq, merged_start)) merged_start = r->seq;
                            if (TCP_SEQ_GT(r->end, merged_end)) merged_end = r->end;
                            if (TCP_SEQ_GT(r->end, r->seq)) old_bytes += r->end - r->seq;
                            overlapping++;
                        }

                        if (!covered && ooo_len) {
                            uint32_t merged_len = merged_end - merged_start;
                            uint32_t increase = merged_len > old_bytes ? merged_len - old_bytes : 0;
                            uint32_t remaining_nodes = overlapping < flow->rx.reass_count ? (uint32_t)(flow->rx.reass_count - overlapping) : 0;

                            tcp_admit_result_t ooo_admit = tcp_admit_ooo(flow, increase, remaining_nodes);
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
                            bool copied = true;

                            if (first > ooo_len) first = ooo_len;
                            if (first && !netpkt_copyout(pkt, ooo_data, rx + pos, first)) copied = false;
                            if (copied && ooo_len > first && !netpkt_copyout(pkt, ooo_data + first, rx, ooo_len - first)) copied = false;

                            if (copied) {
                                for (uint32_t i = 0; i < flow->rx.reass_count;) {
                                    tcp_reass_seg_t *r = &flow->rx.reass[i];
                                    if (TCP_SEQ_LT(r->end, merged_start) || TCP_SEQ_GT(r->seq, merged_end)) {
                                        i++;
                                        continue;
                                    }
                
                                    tcp_reass_remove(flow,i);
                                }

                                if (flow->rx.reass_count < TCP_REASS_MAX_SEGS) {
                                    uint32_t pos_idx = flow->rx.reass_count;
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
                                    tcp_update_adv_wnd(flow, 1);
                                }
                            }
                        }
                    }
                }

                if (fin_in){
                    flow->rx.fin_pending = 1;
                    flow->rx.fin_seq = fin_seq;
                }

                need_ack = true;
                ack_immediate = true;
            }
        }
    }

    if (need_ack){
        if (ack_immediate) tcp_send_ack_now(flow);
        else {
            if (!flow->timer.delayed_ack_pending){
                flow->timer.delayed_ack_pending = 1;
                flow->timer.delayed_ack_timer_ms = 0;
                tcp_daemon_kick();
            } else tcp_send_ack_now(flow);
        }
    }
    tcp_flow_put(flow);
    netpkt_unref(pkt);
}
