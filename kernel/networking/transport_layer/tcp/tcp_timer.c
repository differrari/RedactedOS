#include "tcp_internal.h"
#include "kernel_processes/kprocess_loader.h"
#include "exceptions/irq.h"

static volatile int tcp_daemon_running = 0;
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


    for (uint16_t n = 0; n < tcp_active_count; n++) {
        tcp_flow_t *f = tcp_flows[tcp_active_flows[n]];
        if (!f) continue;
        if (f->base.state == TCP_STATE_CLOSED) continue;

        if (f->base.state == TCP_TIME_WAIT) return 1;
        if (f->base.state == TCP_FIN_WAIT_2) return 1;
        if (f->tx.fin_tx_pending && f->tx.snd_wnd == 0) return 1;
        if (f->tx.nagle_len) return 1;
        if (f->timer.delayed_ack_pending) return 1;
        if (f->timer.persist_active) return 1;
        if (!f->tx.fin_tx_pending && f->timer.keepalive_on && f->base.state == TCP_ESTABLISHED && f->timer.keepalive_ms) return 1;

        for (int j = 0; j < TCP_MAX_TX_SEGS; j++) {
            tcp_tx_seg_t *seg = &f->tx.txq[j];
            if (!seg->used) continue;
            uint32_t end_seq = seg->seq + seg->len + (seg->syn ? 1u : 0u) + (seg->fin ? 1u : 0u);
            if (end_seq > f->tx.snd_una) return 1;
        }
    }

    return 0;
}

void tcp_tick_all(uint32_t elapsed_ms) {
    for (uint16_t pos = tcp_active_count; pos > 0; pos--) {
        int i = tcp_active_flows[pos-1];
        tcp_flow_t *f = tcp_flows[i];
        if (!f) continue;
        if (f->base.state == TCP_STATE_CLOSED) continue;

        if (f->base.state == TCP_TIME_WAIT) {
            tcp_release_io_buffers(f);
            f->timer.time_wait_ms += elapsed_ms;
            if (f->timer.time_wait_ms >= TCP_2MSL_MS) {
                tcp_free_flow(i);
                continue;
            }
        }

        if (f->base.state == TCP_FIN_WAIT_2) {
            f->timer.fin_wait2_ms += elapsed_ms;
            if (f->timer.fin_wait2_ms >= TCP_2MSL_MS) {
                tcp_free_flow(i);
                continue;
            }
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

        if (!f->tx.fin_tx_pending && f->timer.keepalive_on && f->base.state == TCP_ESTABLISHED && f->timer.keepalive_ms) {
            f->timer.keepalive_idle_ms += elapsed_ms;
            if (f->timer.keepalive_idle_ms >= f->timer.keepalive_ms) {
                tcp_hdr_t hdr;
                hdr.src_port = bswap16(f->base.local_port);
                hdr.dst_port = bswap16(f->base.remote.port);
                uint32_t seq = f->tx.snd_nxt;
                if (seq) seq -= 1;
                hdr.sequence = bswap32(seq);
                hdr.ack = bswap32(f->base.ctx.ack);
                hdr.flags = (uint8_t)(1u << ACK_F);
                hdr.window = tcp_calc_adv_wnd_field(f, 1);
                hdr.urgent_ptr = 0;

                if (f->base.local.ver == IP_VER4) {
                    ipv4_tx_opts_t tx;
                    tcp_build_tx_opts_from_local_v4(f->base.local.ip, &tx);
                    (void)tcp_send_segment(IP_VER4, f->base.local.ip, f->base.remote.ip, &hdr, NULL, 0, NULL, 0, (const ip_tx_opts_t *)&tx, f->ip.ttl, f->ip.dontfrag);
                } else if (f->base.local.ver == IP_VER6) {
                    ipv6_tx_opts_t tx;
                    tcp_build_tx_opts_from_local_v6(f->base.local.ip, &tx);
                    (void)tcp_send_segment(IP_VER6, f->base.local.ip, f->base.remote.ip, &hdr, NULL, 0, NULL, 0, (const ip_tx_opts_t *)&tx, f->ip.ttl, f->ip.dontfrag);
                }
                f->timer.keepalive_idle_ms = 0;
            }
        }

        if (f->tx.snd_wnd == 0 && (f->tx.snd_nxt > f->tx.snd_una || f->tx.fin_tx_pending)) {
            if (!f->timer.persist_active) {
                f->timer.persist_active = 1;
                f->timer.persist_timer_ms = 0;
                f->timer.persist_probe_cnt = 0;
                f->timer.persist_timeout_ms = TCP_PERSIST_MIN_MS;
            } else {
                f->timer.persist_timer_ms += elapsed_ms;
                if (f->timer.persist_timer_ms >= f->timer.persist_timeout_ms) {
                    if (f->tx.fin_tx_pending && f->timer.persist_probe_cnt >= TCP_MAX_PERSIST_PROBES) {
                        if (f->base.state == TCP_ESTABLISHED || f->base.state == TCP_CLOSE_WAIT) tcp_free_flow(i);
                        else tcp_free_flow(i);
                        continue;
                    }
                    tcp_tx_seg_t *best = tcp_find_first_unacked(f);

                    tcp_hdr_t hdr;
                    hdr.src_port = bswap16(f->base.local_port);
                    hdr.dst_port = bswap16(f->base.remote.port);

                    uint8_t payload[1];
                    const uint8_t *pp = NULL;
                    uint16_t pl = 0;

                    uint32_t probe_seq = f->tx.snd_una;
                    if (!best && f->tx.fin_tx_pending && f->tx.snd_nxt == f->tx.snd_una && f->tx.snd_nxt) probe_seq = f->tx.snd_nxt-1;

                    if (best && best->buf && best->len && probe_seq >= best->seq && probe_seq < best->seq + best->len) {
                        payload[0] = *((uint8_t *)best->buf + (probe_seq - best->seq));
                        pp = payload;
                        pl = 1;
                    }

                    hdr.sequence = bswap32(probe_seq);
                    hdr.ack = bswap32(f->base.ctx.ack);
                    hdr.flags = (uint8_t)(1u << ACK_F);
                    hdr.window = tcp_calc_adv_wnd_field(f, 1);
                    hdr.urgent_ptr = 0;

                    if (f->base.local.ver == IP_VER4) {
                        ipv4_tx_opts_t tx;
                        tcp_build_tx_opts_from_local_v4(f->base.local.ip, &tx);
                        (void)tcp_send_segment(IP_VER4, f->base.local.ip, f->base.remote.ip, &hdr, NULL, 0, pp, pl, (const ip_tx_opts_t *)&tx, f->ip.ttl, f->ip.dontfrag);
                    } else if (f->base.local.ver == IP_VER6) {
                        ipv6_tx_opts_t tx;
                        tcp_build_tx_opts_from_local_v6(f->base.local.ip, &tx);
                        (void)tcp_send_segment(IP_VER6, f->base.local.ip, f->base.remote.ip, &hdr, NULL, 0, pp, pl, (const ip_tx_opts_t *)&tx, f->ip.ttl, f->ip.dontfrag);
                    }

                    if (f->timer.persist_probe_cnt < UINT8_MAX) f->timer.persist_probe_cnt++;
                    f->timer.persist_timer_ms = 0;

                    if (f->timer.persist_timeout_ms < TCP_PERSIST_MAX_MS) {
                        uint32_t next = f->timer.persist_timeout_ms << 1;
                        if (next > TCP_PERSIST_MAX_MS) next = TCP_PERSIST_MAX_MS;
                        f->timer.persist_timeout_ms = next;
                    }
                }
            }
        } else {
            f->timer.persist_active = 0;
            f->timer.persist_timer_ms = 0;
            f->timer.persist_timeout_ms = 0;
            f->timer.persist_probe_cnt = 0;
        }

        for (int j = 0; j < TCP_MAX_TX_SEGS; j++) {
            tcp_tx_seg_t *s = &f->tx.txq[j];
            if (!s->used) continue;

            s->timer_ms += elapsed_ms;
            if (s->timer_ms < s->timeout_ms) continue;
            if (f->tx.snd_wnd == 0 && s->len > 0) continue;

            if (s->retransmit_cnt >= TCP_MAX_RETRANS) {
                if (f->base.state == TCP_ESTABLISHED || f->base.state == TCP_CLOSE_WAIT) tcp_free_flow(i);
                else tcp_free_flow(i);
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
