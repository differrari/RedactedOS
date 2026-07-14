#include "tcp_internal.h"
#include "kernel_processes/kprocess_loader.h"
#include "exceptions/irq.h"

static volatile int tcp_daemon_running = 0;

static void tcp_timer_send_ack_segment(tcp_flow_t *f, uint32_t seq, const uint8_t *payload, uint16_t payload_len) {
    tcp_hdr_t hdr;
    hdr.src_port = bswap16(f->base.local_port);
    hdr.dst_port = bswap16(f->base.remote.port);
    hdr.sequence = bswap32(seq);
    hdr.ack = bswap32(f->base.ctx.ack);
    hdr.flags = (uint8_t)(1u << ACK_F);
    hdr.window = tcp_calc_adv_wnd_field(f, 1);
    hdr.urgent_ptr = 0;

    if (f->base.local.ver == IP_VER4) {
        ip_tx_opts_t tx;
        tcp_build_tx_opts_from_local_v4(f->base.local.ip, &tx);
        (void)tcp_send_segment(IP_VER4, f->base.local.ip, f->base.remote.ip, &hdr, NULL, 0, payload, payload_len, &tx, f->ip.ttl, f->ip.dontfrag);
    } else if (f->base.local.ver == IP_VER6) {
        ip_tx_opts_t tx;
        tcp_build_tx_opts_from_local_v6(f->base.local.ip, &tx);
        (void)tcp_send_segment(IP_VER6, f->base.local.ip, f->base.remote.ip, &hdr, NULL, 0, payload, payload_len, &tx, f->ip.ttl, f->ip.dontfrag);
    }
}
//TODO events
void tcp_daemon_kick(void) {
    if(!tcp_has_pending_timers()) return;

    irq_flags_t irq_flags = irq_save_disable();
    if(tcp_daemon_running){
        irq_restore(irq_flags);
        return;
    }
    tcp_daemon_running = 1;
    irq_restore(irq_flags);

    process_t *p = create_kernel_process("tcp_timer", tcp_daemon_entry, 0, 0);
    if(!p){
        irq_flags = irq_save_disable();
        tcp_daemon_running = 0;
        irq_restore(irq_flags);
    }
}

int tcp_has_pending_timers(void) { //TODO mhh this should be event driven to avoid MAX_TCP_FLOWS*TCP_MAX_TX_SEGS scans.
    irq_flags_t irq = irq_save_disable();

    for (uint16_t n = 0; n < tcp_active_count; n++) {
        tcp_flow_t *f = tcp_flows[tcp_active_flows[n]];
        if (!f) continue;
        if (f->base.retired || f->base.state == TCP_STATE_CLOSED) continue;

        if (f->base.state == TCP_TIME_WAIT || f->base.state == TCP_FIN_WAIT_2 || f->tx.fin_tx_pending || f->tx.nagle_len || f->timer.delayed_ack_pending || f->timer.persist_active || (!f->tx.fin_tx_pending && f->timer.keepalive_on && f->base.state == TCP_ESTABLISHED && f->timer.keepalive_ms)) {
            irq_restore(irq);
            return 1;
        }

        for (int j = 0; j < TCP_MAX_TX_SEGS; j++) {
            tcp_tx_seg_t *seg = &f->tx.txq[j];
            if (!seg->used) continue;
            uint32_t end_seq = seg->seq + seg->len + (seg->syn ? 1u : 0u) + (seg->fin ? 1u : 0u);
            if (TCP_SEQ_GT(end_seq, f->tx.snd_una)) {
                irq_restore(irq);
                return 1;
            }
        }
    }

    irq_restore(irq);
    return 0;
}

void tcp_tick_all(uint32_t elapsed_ms) {
    for (uint16_t pos = tcp_active_count; pos > 0; pos--) {
        irq_flags_t irq = irq_save_disable();
        if (pos > tcp_active_count) {
            irq_restore(irq); 
            continue;
        }

        int i = tcp_active_flows[pos-1];
        tcp_flow_t *f = tcp_flows[i];
        if (!f || f->base.retired || f->base.state == TCP_STATE_CLOSED || !f->base.refs || f->base.refs == UINT16_MAX) {
            irq_restore(irq); 
            continue;
        }
        f->base.refs++;
        irq_restore(irq);
        int retire_flow = 0;

        if (f->base.state == TCP_TIME_WAIT) {
            f->timer.time_wait_ms += elapsed_ms;
            if (f->timer.time_wait_ms >= TCP_2MSL_MS) retire_flow = 1;
        }

        if (!retire_flow && f->base.state == TCP_FIN_WAIT_2) {
            f->timer.fin_wait2_ms += elapsed_ms;
            if (f->timer.fin_wait2_ms >= TCP_2MSL_MS) retire_flow = 1;
        }

        if (retire_flow) {
            tcp_free_flow(i);
            tcp_flow_put(f);
            continue;
        }

        if (f->timer.delayed_ack_pending) {
            f->timer.delayed_ack_timer_ms += elapsed_ms;
            if (f->timer.delayed_ack_timer_ms >= TCP_DELAYED_ACK_MS) tcp_send_ack_now(f);
        }

        if (f->tx.nagle_len) {
            if (f->tx.snd_nxt == f->tx.snd_una) tcp_flush_nagle(f, 1);
            else {
                f->tx.nagle_timer_ms += elapsed_ms;
                if (f->tx.nagle_timer_ms >= TCP_NAGLE_TIMEOUT_MS) tcp_flush_nagle(f, 1);
            }
        }

        if (f->tx.fin_tx_pending) tcp_try_send_pending_fin(f);

        if (!f->tx.fin_tx_pending && f->timer.keepalive_on && f->base.state == TCP_ESTABLISHED && f->timer.keepalive_ms) {
            f->timer.keepalive_idle_ms += elapsed_ms;
            if (f->timer.keepalive_idle_ms >= f->timer.keepalive_ms) {
                uint32_t seq = f->tx.snd_nxt;
                if (seq) seq -= 1;
                tcp_timer_send_ack_segment(f, seq, NULL, 0);
                f->timer.keepalive_idle_ms = 0;
            }
        }

        if (f->tx.snd_wnd == 0 && (TCP_SEQ_GT(f->tx.snd_nxt, f->tx.snd_una) || f->tx.fin_tx_pending)) {
            if (!f->timer.persist_active) {
                f->timer.persist_active = 1;
                f->timer.persist_timer_ms = 0;
                f->timer.persist_probe_cnt = 0;
                f->timer.persist_timeout_ms = TCP_PERSIST_MIN_MS;
            } else {
                f->timer.persist_timer_ms += elapsed_ms;
                if (f->timer.persist_timer_ms >= f->timer.persist_timeout_ms) {
                    if (f->tx.fin_tx_pending && f->timer.persist_probe_cnt >= TCP_MAX_PERSIST_PROBES) {
                        retire_flow = 1;
                    } else {
                        tcp_tx_seg_t *best = tcp_find_first_unacked(f);

                        uint8_t payload[1];
                        const uint8_t *pp = NULL;
                        uint16_t pl = 0;

                        uint32_t probe_seq = f->tx.snd_una;
                        if (!best && f->tx.fin_tx_pending && f->tx.snd_nxt == f->tx.snd_una && f->tx.snd_nxt) probe_seq = f->tx.snd_nxt-1;

                        const uint8_t *best_payload = tcp_tx_seg_payload_ptr(best);
                        if (best && best_payload && best->len && TCP_SEQ_GEQ(probe_seq, best->seq) && TCP_SEQ_LT(probe_seq, best->seq + best->len)) {
                            payload[0] = best_payload[probe_seq - best->seq];
                            pp = payload;
                            pl = 1;
                        }

                        tcp_timer_send_ack_segment(f, probe_seq, pp, pl);

                        if (f->timer.persist_probe_cnt < UINT8_MAX) f->timer.persist_probe_cnt++;
                        f->timer.persist_timer_ms = 0;

                        if (f->timer.persist_timeout_ms < TCP_PERSIST_MAX_MS) {
                            uint32_t next = f->timer.persist_timeout_ms << 1;
                            if (next > TCP_PERSIST_MAX_MS) next = TCP_PERSIST_MAX_MS;
                            f->timer.persist_timeout_ms = next;
                        }
                    }
                }
            }
        } else {
            f->timer.persist_active = 0;
            f->timer.persist_timer_ms = 0;
            f->timer.persist_timeout_ms = 0;
            f->timer.persist_probe_cnt = 0;
        }

        if (retire_flow) {
            tcp_free_flow(i);
            tcp_flow_put(f);
            continue;
        }

        for (int j = 0; j < TCP_MAX_TX_SEGS; j++) {
            tcp_tx_seg_t *s = &f->tx.txq[j];
            if (!s->used) continue;

            s->timer_ms += elapsed_ms;
            if (s->timer_ms < s->timeout_ms) continue;
            if (f->tx.snd_wnd == 0 && s->len > 0) continue;

            if (s->retransmit_cnt >= TCP_MAX_RETRANS) {
                retire_flow = 1;
                break;
            }

            tcp_cc_on_timeout(f);

            tcp_send_from_seg(f, s);

            s->retransmit_cnt++;
            s->timer_ms = 0;

            if (s->timeout_ms == 0) {
                uint32_t rto = f->tx.rto ? f->tx.rto : TCP_INIT_RTO;
                if (rto < TCP_MIN_RTO) rto = TCP_MIN_RTO;
                s->timeout_ms = rto;
            } else if (s->timeout_ms < TCP_MAX_RTO) {
                uint32_t next = s->timeout_ms << 1;
                if (next > TCP_MAX_RTO) next = TCP_MAX_RTO;
                s->timeout_ms = next;
            }
        }

        if (retire_flow) {
            tcp_free_flow(i);
            tcp_flow_put(f);
            continue;
        }

        tcp_flow_put(f);
    }
}


int tcp_daemon_entry(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    const uint32_t tick_ms = 25;

    while (1) {
        if (tcp_has_pending_timers()) tcp_tick_all(tick_ms);
        msleep(tick_ms);
    }

    return 0;
}
