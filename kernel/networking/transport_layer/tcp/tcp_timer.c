#include "tcp_internal.h"
#include "kernel_processes/kprocess_loader.h"
#include "exceptions/irq.h"

static volatile int tcp_daemon_running;
static int tcp_daemon_entry(int argc, char *argv[]);

static bool tcp_timer_send_ack_segment(tcp_flow_t *flow, uint32_t seq, const uint8_t *payload, uint16_t payload_len) {
    tcp_hdr_t hdr;
    hdr.src_port = bswap16(flow->base.local.port);
    hdr.dst_port = bswap16(flow->base.remote.port);
    hdr.sequence = bswap32(seq);
    hdr.ack = bswap32(flow->base.ctx.ack);
    hdr.flags = (uint8_t)(1u << ACK_F);
    tcp_update_adv_wnd(flow, 1);
    hdr.window = flow->base.ctx.window;
    hdr.urgent_ptr = 0;
    return tcp_send_flow_segment(flow, &hdr, NULL, 0, payload, payload_len);
}

//TODO events
void tcp_daemon_kick(void) {
    irq_flags_t irq_flags = irq_save_disable();
    if(tcp_daemon_running){
        irq_restore(irq_flags);
        return;
    }
    tcp_daemon_running = 1;
    irq_restore(irq_flags);

    if (!create_kernel_process("tcp_timer", tcp_daemon_entry, 0, 0)) {
        irq_flags = irq_save_disable();
        tcp_daemon_running = 0;
        irq_restore(irq_flags);
    }
}

static int tcp_daemon_entry(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    uint32_t last_tick_ms = (uint32_t)get_time();

    while (true) {
        uint32_t now_ms = (uint32_t)get_time();
        uint32_t elapsed_ms = now_ms - last_tick_ms;
        last_tick_ms = now_ms;

        uint16_t active_slots[MAX_TCP_FLOWS];
        uint32_t active_generations[MAX_TCP_FLOWS];
        uint16_t active_count;

        irq_flags_t irq = irq_save_disable();
        active_count = tcp_active_count;
        if (!active_count) {
            tcp_daemon_running = 0;
            irq_restore(irq);
            return 0;
        }
        for (uint16_t i = 0; i < active_count; ++i) {
            uint16_t slot = tcp_active_flows[i];
            tcp_flow_t* flow = slot < MAX_TCP_FLOWS ? tcp_flows[slot] : NULL;
            active_slots[i] = slot;
            active_generations[i] = flow ? flow->base.generation : 0;
        }
        irq_restore(irq);

        for (uint16_t pos = 0; pos < active_count; ++pos) {
            irq = irq_save_disable();
            uint16_t slot = active_slots[pos];
            tcp_flow_t *flow = slot < MAX_TCP_FLOWS ? tcp_flows[slot] : NULL;
            if (!flow || flow->base.generation != active_generations[pos] || flow->base.retired || flow->base.state == TCP_STATE_CLOSED || !flow->base.refs || flow->base.refs == UINT16_MAX) {
                irq_restore(irq);
                continue;
            }
            flow->base.refs++;
            irq_restore(irq);
            bool retire_flow = false;

            if (flow->base.state == TCP_TIME_WAIT) {
                flow->timer.time_wait_ms += elapsed_ms;
                if (flow->timer.time_wait_ms >= TCP_2MSL_MS) retire_flow = true;
            }

            if (!retire_flow && flow->base.state == TCP_FIN_WAIT_2) {
                flow->timer.fin_wait2_ms += elapsed_ms;
                if (flow->timer.fin_wait2_ms >= TCP_2MSL_MS) retire_flow = true;
            }

            if (retire_flow) {
                tcp_free_flow(flow);
                tcp_flow_put(flow);
                continue;
            }

            if (flow->timer.delayed_ack_pending) {
                flow->timer.delayed_ack_timer_ms += elapsed_ms;
                if (flow->timer.delayed_ack_timer_ms >= TCP_DELAYED_ACK_MS) tcp_send_ack_now(flow);
            }

            if (flow->tx.snd_wnd > 0) {
                tcp_tx_seg_t *best = tcp_find_first_unacked(flow);
                if (best && best->persist) {
                    best->persist = 0;
                    if (!tcp_retransmit_seg(flow, best)) {
                        best->timer_ms = 0;
                        best->timeout_ms = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
                    }
                }
            }

            if (flow->tx.nagle_len && flow->tx.snd_wnd > 0) {
                if (flow->tx.snd_nxt == flow->tx.snd_una) {
                    if (!tcp_flush_nagle(flow, 1)) flow->tx.nagle_timer_ms = 0;
                } else {
                    flow->tx.nagle_timer_ms += elapsed_ms;
                    if (flow->tx.nagle_timer_ms >= TCP_NAGLE_TIMEOUT_MS && !tcp_flush_nagle(flow, 1)) flow->tx.nagle_timer_ms = 0;
                }
            }

            if (flow->tx.fin_tx_pending) tcp_try_send_pending_fin(flow);

            if (!flow->tx.fin_tx_pending && !flow->tx.nagle_len && flow->timer.keepalive_on && flow->base.state == TCP_ESTABLISHED && flow->timer.keepalive_ms && flow->tx.snd_nxt == flow->tx.snd_una) {
                flow->timer.keepalive_idle_ms += elapsed_ms;
                if (flow->timer.keepalive_idle_ms >= flow->timer.keepalive_ms) {
                    uint32_t seq = flow->tx.snd_nxt;
                    if (seq) seq--;
                    bool sent = tcp_timer_send_ack_segment(flow, seq, NULL, 0);
                    flow->timer.keepalive_idle_ms = sent || flow->timer.keepalive_ms <= TCP_MIN_RTO ? 0 : flow->timer.keepalive_ms - TCP_MIN_RTO;
                }
            }

            bool persist_state = flow->base.state == TCP_ESTABLISHED || flow->base.state == TCP_CLOSE_WAIT || flow->base.state == TCP_FIN_WAIT_1 || flow->base.state == TCP_FIN_WAIT_2 || flow->base.state == TCP_CLOSING || flow->base.state == TCP_LAST_ACK;
            if (persist_state && flow->tx.snd_wnd == 0 && (TCP_SEQ_GT(flow->tx.snd_nxt, flow->tx.snd_una) || flow->tx.nagle_len || flow->tx.fin_tx_pending)) {
                if (!flow->timer.persist_active) {
                    flow->timer.persist_active = 1;
                    flow->timer.persist_timer_ms = 0;
                    flow->timer.persist_timeout_ms = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
                    if (flow->timer.persist_timeout_ms < TCP_PERSIST_MIN_MS) flow->timer.persist_timeout_ms = TCP_PERSIST_MIN_MS;
                    if (flow->timer.persist_timeout_ms > TCP_PERSIST_MAX_MS) flow->timer.persist_timeout_ms = TCP_PERSIST_MAX_MS;
                } else {
                    flow->timer.persist_timer_ms += elapsed_ms;
                    if (flow->timer.persist_timer_ms >= flow->timer.persist_timeout_ms) {
                        tcp_tx_seg_t *best = tcp_find_first_unacked(flow);

                        uint8_t payload = 0;
                        const uint8_t *probe_payload = NULL;
                        uint16_t probe_len = 0;

                        uint32_t probe_seq = flow->tx.snd_una;
                        if (!best && flow->tx.fin_tx_pending && flow->tx.snd_nxt == flow->tx.snd_una && flow->tx.snd_nxt) probe_seq = flow->tx.snd_nxt - 1;

                        const uint8_t *best_payload = tcp_tx_seg_payload_ptr(best);
                        if (best && best_payload && best->len && TCP_SEQ_GEQ(probe_seq, best->seq) && TCP_SEQ_LT(probe_seq, best->seq + best->len)) {
                            payload = best_payload[probe_seq - best->seq];
                            probe_payload = &payload;
                            probe_len = 1;
                        }

                        bool sent = false;
                        if (!best && flow->tx.nagle_len) sent = tcp_flush_nagle(flow, 1) != 0;
                        else sent = tcp_timer_send_ack_segment(flow, probe_seq, probe_payload, probe_len);
                        flow->timer.persist_timer_ms = 0;

                        if (sent) {
                            if (flow->timer.persist_timeout_ms < TCP_PERSIST_MAX_MS) {
                                uint32_t next = flow->timer.persist_timeout_ms << 1;
                                if (next > TCP_PERSIST_MAX_MS) next = TCP_PERSIST_MAX_MS;
                                flow->timer.persist_timeout_ms = next;
                            }
                        }
                    }
                }
            } else {
                flow->timer.persist_active = 0;
                flow->timer.persist_timer_ms = 0;
                flow->timer.persist_timeout_ms = 0;
            }

            if (TCP_SEQ_GT(flow->tx.snd_nxt, flow->tx.snd_una)) {
                for (uint32_t j = 0; j < TCP_MAX_TX_SEGS; j++) {
                    tcp_tx_seg_t *sample = &flow->tx.txq[j];
                    if (!sample->used || !sample->rtt_sample || sample->persist) continue;
                    sample->rtt_timer_ms += elapsed_ms;
                }

                tcp_tx_seg_t *seg = tcp_find_first_unacked(flow);
                if (seg && !(flow->tx.snd_wnd == 0 && !seg->syn)) {
                    seg->timer_ms += elapsed_ms;
                    if (seg->timer_ms >= seg->timeout_ms) {
                        if (seg->retransmit_cnt >= TCP_MAX_RETRANS) retire_flow = true;
                        else if (!tcp_retransmit_seg(flow, seg)) seg->timer_ms = 0;
                        else {
                            tcp_cc_on_timeout(flow);
                            uint32_t rto = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;
                            if (rto < TCP_MIN_RTO) rto = TCP_MIN_RTO;
                            if (rto < TCP_MAX_RTO) {
                                uint32_t next = rto << 1;
                                if (next > TCP_MAX_RTO) next = TCP_MAX_RTO;
                                rto = next;
                            }
                            flow->tx.rto = rto;
                            seg->timeout_ms = rto;
                        }
                    }
                }
            }

            if (retire_flow) tcp_free_flow(flow);
            tcp_flow_put(flow);
        }

        msleep(10);
    }

    return 0;
}
