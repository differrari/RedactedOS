#include "tcp_internal.h"
#include "kernel_processes/kprocess_loader.h"
#include "exceptions/irq.h"

static volatile int tcp_daemon_running = 0;
//TODO make tcp_daemon_running atomic or use a lock, this may end in a double deamon process
void tcp_daemon_kick(void) {
    if(!tcp_has_pending_timers()) return;

    disable_interrupt();
    if(tcp_daemon_running){
        enable_interrupt();
        return;
    }
    tcp_daemon_running = 1;
    enable_interrupt();

    process_t *p = create_kernel_process("tcp_timer", tcp_daemon_entry, 0, 0);
    if(!p){
        disable_interrupt();
        tcp_daemon_running = 0;
        enable_interrupt();
    }
}

int tcp_has_pending_timers(void) { //TODO mhh this should be event driven to avoid MAX_TCP_FLOWS*TCP_MAX_TX_SEGS scans.


    for (int i = 0; i < MAX_TCP_FLOWS; i++) {
        tcp_flow_t *f = tcp_flows[i];
        if (!f) continue;
        if (f->state == TCP_STATE_CLOSED) continue;

        if (f->state == TCP_TIME_WAIT) return 1;
        if (f->state == TCP_FIN_WAIT_2) return 1;
        if (f->delayed_ack_pending) return 1;
        if (f->persist_active) return 1;
        if (f->keepalive_on && f->state == TCP_ESTABLISHED && f->keepalive_ms) return 1;

        for (int j = 0; j < TCP_MAX_TX_SEGS; j++) {
            tcp_tx_seg_t *s = &f->txq[j];
            if (!s->used) continue;
            uint32_t end = s->seq + s->len + (s->syn ? 1u : 0u) + (s->fin ? 1u : 0u);
            if (end > f->snd_una) return 1;
        }
    }

    return 0;
}

void tcp_tick_all(uint32_t elapsed_ms) {
    for (int i = 0; i < MAX_TCP_FLOWS; i++) {
        tcp_flow_t *f = tcp_flows[i];
        if (!f) continue;
        if (f->state == TCP_STATE_CLOSED) continue;

        if (f->state == TCP_TIME_WAIT) {
            f->time_wait_ms += elapsed_ms;
            if (f->time_wait_ms >= TCP_2MSL_MS) {
                tcp_free_flow(i);
                continue;
            }
        }

        if (f->state == TCP_FIN_WAIT_2) {
            f->fin_wait2_ms += elapsed_ms;
            if (f->fin_wait2_ms >= TCP_2MSL_MS) {
                tcp_free_flow(i);
                continue;
            }
        }

        if (f->delayed_ack_pending) {
            f->delayed_ack_timer_ms += elapsed_ms;
            if (f->delayed_ack_timer_ms >= TCP_DELAYED_ACK_MS) tcp_send_ack_now(f);
        }

        if (f->keepalive_on && f->state == TCP_ESTABLISHED && f->keepalive_ms) {
            f->keepalive_idle_ms += elapsed_ms;
            if (f->keepalive_idle_ms >= f->keepalive_ms) {
                tcp_hdr_t hdr;
                hdr.src_port = bswap16(f->local_port);
                hdr.dst_port = bswap16(f->remote.port);
                uint32_t seq = f->snd_nxt;
                if (seq) seq -= 1;
                hdr.sequence = bswap32(seq);
                hdr.ack = bswap32(f->ctx.ack);
                hdr.flags = (uint8_t)(1u << ACK_F);
                hdr.window = tcp_calc_adv_wnd_field(f, 1);
                hdr.urgent_ptr = 0;

                if (f->local.ver == IP_VER4) {
                    ipv4_tx_opts_t tx;
                    tcp_build_tx_opts_from_local_v4(f->local.ip, &tx);
                    (void)tcp_send_segment(IP_VER4, f->local.ip, f->remote.ip, &hdr, NULL, 0, NULL, 0, (const ip_tx_opts_t *)&tx, f->ip_ttl, f->ip_dontfrag);
                } else if (f->local.ver == IP_VER6) {
                    ipv6_tx_opts_t tx;
                    tcp_build_tx_opts_from_local_v6(f->local.ip, &tx);
                    (void)tcp_send_segment(IP_VER6, f->local.ip, f->remote.ip, &hdr, NULL, 0, NULL, 0, (const ip_tx_opts_t *)&tx, f->ip_ttl, f->ip_dontfrag);
                }
                f->keepalive_idle_ms = 0;
            }
        }

        if (f->snd_wnd == 0 && f->snd_nxt > f->snd_una) {
            if (!f->persist_active) {
                f->persist_active = 1;
                f->persist_timer_ms = 0;
                f->persist_probe_cnt = 0;
                f->persist_timeout_ms = TCP_PERSIST_MIN_MS;
            } else {
                f->persist_timer_ms += elapsed_ms;
                if (f->persist_timer_ms >= f->persist_timeout_ms) {
                    if (f->persist_probe_cnt >= TCP_MAX_PERSIST_PROBES) {
                        if (f->state == TCP_ESTABLISHED) {
                            f->ctx.flags = (uint8_t)((1u << FIN_F) | (1u << ACK_F));
                            f->ctx.payload.ptr = 0;
                            f->ctx.payload.size = 0;

                            tcp_flow_send(&f->ctx);
                            f->state = TCP_FIN_WAIT_1;
                            f->ctx.expected_ack = f->snd_nxt;
                            tcp_daemon_kick();
                        } else {
                            tcp_free_flow(i);
                        }
                        continue;
                    }
                    tcp_tx_seg_t *best = tcp_find_first_unacked(f);

                    tcp_hdr_t hdr;
                    hdr.src_port = bswap16(f->local_port);
                    hdr.dst_port = bswap16(f->remote.port);

                    uint8_t payload[1];
                    const uint8_t *pp = NULL;
                    uint16_t pl = 0;

                    uint32_t probe_seq = f->snd_una;

                    if (best && best->buf && best->len && probe_seq >= best->seq && probe_seq < best->seq + best->len) {
                        payload[0] = *((uint8_t *)best->buf + (probe_seq - best->seq));
                        pp = payload;
                        pl = 1;
                    }

                    hdr.sequence = bswap32(probe_seq);
                    hdr.ack = bswap32(f->ctx.ack);
                    hdr.flags = (uint8_t)(1u << ACK_F);
                    hdr.window = tcp_calc_adv_wnd_field(f, 1);
                    hdr.urgent_ptr = 0;

                    if (f->local.ver == IP_VER4) {
                        ipv4_tx_opts_t tx;
                        tcp_build_tx_opts_from_local_v4(f->local.ip, &tx);
                        (void)tcp_send_segment(IP_VER4, f->local.ip, f->remote.ip, &hdr, NULL, 0, pp, pl, (const ip_tx_opts_t *)&tx, f->ip_ttl, f->ip_dontfrag);
                    } else if (f->local.ver == IP_VER6) {
                        ipv6_tx_opts_t tx;
                        tcp_build_tx_opts_from_local_v6(f->local.ip, &tx);
                        (void)tcp_send_segment(IP_VER6, f->local.ip, f->remote.ip, &hdr, NULL, 0, pp, pl, (const ip_tx_opts_t *)&tx, f->ip_ttl, f->ip_dontfrag);
                    }

                    if (f->persist_probe_cnt < UINT8_MAX) f->persist_probe_cnt++;
                    f->persist_timer_ms = 0;

                    if (f->persist_timeout_ms < TCP_PERSIST_MAX_MS) {
                        uint32_t next = f->persist_timeout_ms << 1;
                        if (next > TCP_PERSIST_MAX_MS) next = TCP_PERSIST_MAX_MS;
                        f->persist_timeout_ms = next;
                    }
                }
            }
        } else {
            f->persist_active = 0;
            f->persist_timer_ms = 0;
            f->persist_timeout_ms = 0;
            f->persist_probe_cnt = 0;
        }

        for (int j = 0; j < TCP_MAX_TX_SEGS; j++) {
            tcp_tx_seg_t *s = &f->txq[j];
            if (!s->used) continue;

            s->timer_ms += elapsed_ms;
            if (s->timer_ms < s->timeout_ms) continue;

            if (s->retransmit_cnt >= TCP_MAX_RETRANS) {
                tcp_free_flow(i);
                break;
            }

            tcp_cc_on_timeout(f);

            tcp_send_from_seg(f, s);

            s->retransmit_cnt++;
            s->timer_ms = 0;

            if (s->timeout_ms == 0) {
                uint32_t rto = f->rto ? f->rto : TCP_INIT_RTO;
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
    const uint32_t grace_ms = 10000;
    uint32_t idle_ms = 0;

    while (1) {
        if (tcp_has_pending_timers()) {
            tcp_tick_all(tick_ms);
            idle_ms = 0;
        } else {
            idle_ms += tick_ms;
            if(idle_ms >= grace_ms) break;
        }
        msleep(tick_ms);
    }

    disable_interrupt();
    tcp_daemon_running = 0;
    enable_interrupt();
    return 0;
}
