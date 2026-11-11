#include "tcp_internal.h"
#include "kernel_processes/kprocess_loader.h"
#include "exceptions/irq.h"
#include "networking/internet_layer/pmtu.h"

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
    bool sent = tcp_send_flow_segment(flow, &hdr, NULL, 0, payload, payload_len);
    if (sent) {
        flow->timer.delayed_ack_pending = 0;
        flow->timer.delayed_ack_timer_ms = 0;
        flow->timer.delayed_ack_bytes = 0;
    }
    return sent;
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
    uint32_t last_pmtu_check_ms = last_tick_ms;

    while (true) {
        uint32_t now_ms = (uint32_t)get_time();
        uint32_t elapsed_ms = now_ms - last_tick_ms;
        last_tick_ms = now_ms;
        bool check_pmtu = now_ms - last_pmtu_check_ms >= 60000;
        if (check_pmtu) last_pmtu_check_ms = now_ms;

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
            int32_t retire_error = SOCK_ERR_TIMED_OUT;

            if (check_pmtu && (flow->base.state == TCP_ESTABLISHED || flow->base.state == TCP_CLOSE_WAIT) && l3_generation(flow->base.l3_id) == flow->base.l3_generation) { 
                bool ready = false;
                if (flow->base.remote.ver == IP_VER4) {
                    l3_ipv4_interface_t *l3 = l3_ipv4_find_by_id(flow->base.l3_id);
                    ready = ipv4_l3_is_ready(l3) && l3_ipv4_effective_mtu(l3) > sizeof(ipv4_hdr_t) + sizeof(tcp_hdr_t);
                } else if (flow->base.remote.ver == IP_VER6) {
                    l3_ipv6_interface_t *l3 = l3_ipv6_find_by_id(flow->base.l3_id);
                    ready = ipv6_l3_is_ready(l3) && l3_ipv6_effective_mtu(l3) > sizeof(ipv6_hdr_t) + sizeof(tcp_hdr_t);
                }
                if (ready) {
                    uint32_t path_mss = tcp_calc_mss_for_l3(flow->base.l3_id, flow->base.remote.ver, flow->base.remote.ip);
                    irq = irq_save_disable();
                    if (path_mss && (path_mss < flow->tx.path_mss || (path_mss > flow->tx.path_mss && now_ms - flow->tx.pmtu_last_note_ms >= PMTU_CACHE_MAX_AGE_MS))) {
                        if (path_mss < flow->tx.path_mss) flow->tx.pmtu_last_note_ms = now_ms;
                        flow->tx.path_mss = path_mss;
                        tcp_update_mss(flow);
                    }
                    irq_restore(irq);
                }
            }

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

            if (flow->tx.cork && flow->tx.nagle_len && flow->tx.snd_wnd > 0) {
                uint32_t left = UINT32_MAX - flow->tx.cork_ms;
                if (elapsed_ms >= left) flow->tx.cork_ms = UINT32_MAX;
                else flow->tx.cork_ms += elapsed_ms;
                if (flow->tx.cork_ms >= TCP_CORK_MAX_HOLD_MS) {
                    tcp_flush_nagle(flow, TCP_TX_BYPASS_NAGLE | TCP_TX_BYPASS_SWS);
                    flow->tx.cork_ms = 0;
                }
            } else flow->tx.cork_ms = 0;

            if (flow->tx.nagle_len && flow->tx.snd_wnd > 0 && !flow->tx.cork) {
                tcp_tx_force_t force = flow->tx.nodelay ? TCP_TX_BYPASS_NAGLE : TCP_TX_FORCE_NONE;
                uint64_t sent = tcp_flush_nagle(flow, force);
                if (!sent && flow->tx.nagle_len) {
                    uint64_t in_flight = TCP_SEQ_GT(flow->tx.snd_nxt, flow->tx.snd_una) ? flow->tx.snd_nxt - flow->tx.snd_una : 0;
                    uint32_t mss = flow->tx.mss ? flow->tx.mss : TCP_DEFAULT_MSS;
                    uint64_t wnd_room = in_flight < flow->tx.snd_wnd ? flow->tx.snd_wnd - in_flight : 0;
                    uint64_t cwnd = flow->tx.cwnd ? flow->tx.cwnd : mss;
                    uint64_t cwnd_room = in_flight < cwnd ? cwnd - in_flight : 0;
                    uint64_t send_room = wnd_room < cwnd_room ? wnd_room : cwnd_room;
                    uint64_t candidate = flow->tx.nagle_len < send_room ? flow->tx.nagle_len : send_room;
                    bool nagle_blocked = !flow->tx.nodelay && in_flight && candidate < mss;
                    if (nagle_blocked) flow->tx.sws_timer_ms = 0;
                    else {
                        flow->tx.sws_timer_ms += elapsed_ms;
                        if (flow->tx.sws_timer_ms >= TCP_SWS_OVERRIDE_MS) {
                            force = (tcp_tx_force_t)(force | TCP_TX_BYPASS_SWS);
                            if (tcp_flush_nagle(flow, force)) flow->tx.sws_timer_ms = 0;
                        }
                    }
                } else flow->tx.sws_timer_ms = 0;
            } else flow->tx.sws_timer_ms = 0;

            if (flow->tx.fin_tx_pending) tcp_try_send_pending_fin(flow);

            if (!flow->tx.fin_tx_pending && !flow->tx.nagle_len && flow->timer.keepalive_on && flow->base.state == TCP_ESTABLISHED && flow->timer.keepalive_ms && flow->tx.snd_nxt == flow->tx.snd_una) {
                uint32_t limit = flow->timer.keepalive_probes ? flow->timer.keepalive_interval_ms : flow->timer.keepalive_ms;
                if (limit < 1000) limit = 1000;
                uint32_t remaining = UINT32_MAX - flow->timer.keepalive_idle_ms;
                if (elapsed_ms >= remaining) flow->timer.keepalive_idle_ms = UINT32_MAX;
                else flow->timer.keepalive_idle_ms += elapsed_ms;
                if (flow->timer.keepalive_idle_ms >= limit) {
                    if (flow->timer.keepalive_probes >= flow->timer.keepalive_count && !flow->timer.user_timeout_ms) retire_flow = true;
                    else {
                        uint32_t seq = flow->tx.snd_nxt ? flow->tx.snd_nxt-1 : 0;
                        if (tcp_timer_send_ack_segment(flow, seq, NULL, 0)){
                            flow->timer.keepalive_probes++;
                            flow->timer.keepalive_idle_ms = 0;
                        } else { 
                            uint32_t retry_ms = flow->timer.keepalive_interval_ms;
                            if (retry_ms < 1000) retry_ms = 1000;
                            if (limit > retry_ms) flow->timer.keepalive_idle_ms = limit - retry_ms;
                            else flow->timer.keepalive_idle_ms = 0;
                        }
                    }
                }
            } else if (!flow->timer.keepalive_probes) flow->timer.keepalive_idle_ms = 0;
            
            if (retire_flow) {
                tcp_flow_note_socket_error(flow, SOCK_ERR_TIMED_OUT);
                tcp_free_flow(flow);
                tcp_flow_put(flow);
                continue;
            }

            bool persist_state = flow->base.state == TCP_ESTABLISHED || flow->base.state == TCP_CLOSE_WAIT || flow->base.state == TCP_FIN_WAIT_1 || flow->base.state == TCP_FIN_WAIT_2 || flow->base.state == TCP_CLOSING || flow->base.state == TCP_LAST_ACK;
            if (persist_state && flow->tx.snd_wnd == 0 && (TCP_SEQ_GT(flow->tx.snd_nxt, flow->tx.snd_una) || flow->tx.nagle_len || flow->tx.fin_tx_pending)) {
                if (!flow->timer.persist_active) tcp_persist_start(flow);
                else {
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
                        if (!best && flow->tx.nagle_len) sent = tcp_flush_nagle(flow, TCP_TX_PERSIST_PROBE) != 0;
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
            } else tcp_persist_reset(flow);

            if (flow->timer.user_timeout_ms && persist_state) {
                uint32_t oldest_ms = 0;
                uint32_t current_ms = (uint32_t)get_time();
                if (TCP_SEQ_GT(flow->tx.snd_nxt, flow->tx.snd_una)) {
                    for (uint32_t i = 0; i < TCP_MAX_TX_SEGS; i++) {
                        tcp_tx_seg_t *seg = &flow->tx.txq[i];
                        if (!seg->used || !seg->first_sent_valid) continue;
                        uint32_t age_ms = current_ms - seg->first_sent_ms;
                        if (age_ms > oldest_ms) oldest_ms = age_ms;
                    }
                }
            
                bool waiting = (flow->tx.snd_wnd == 0 && flow->tx.nagle_len) || (flow->timer.keepalive_on && flow->timer.keepalive_probes);
                if (waiting) {
                    uint32_t left = UINT32_MAX - flow->timer.user_timeout_wait_ms;
                    if (elapsed_ms >= left) flow->timer.user_timeout_wait_ms = UINT32_MAX;
                    else flow->timer.user_timeout_wait_ms += elapsed_ms;
                } else flow->timer.user_timeout_wait_ms = 0;

                if (oldest_ms >= flow->timer.user_timeout_ms || (waiting && flow->timer.user_timeout_wait_ms >= flow->timer.user_timeout_ms)) {
                    tcp_flow_note_socket_error(flow, SOCK_ERR_TIMED_OUT);
                    tcp_free_flow(flow);
                    tcp_flow_put(flow);
                    continue;
                }
            } else flow->timer.user_timeout_wait_ms = 0;

            if (TCP_SEQ_GT(flow->tx.snd_nxt, flow->tx.snd_una)) {
                tcp_tx_seg_t *seg = tcp_find_first_unacked(flow);
                if (seg && !(flow->tx.snd_wnd == 0 && !seg->syn)) {
                    seg->timer_ms += elapsed_ms;
                    if (seg->timer_ms >= seg->timeout_ms) {
                        uint8_t retries = seg->syn && flow->base.state == TCP_SYN_SENT && flow->tx.syn_retries ? flow->tx.syn_retries : TCP_MAX_RETRANS;
                        if (seg->retransmit_cnt >= retries) retire_flow = true;
                        else {
                            bool sent = tcp_retransmit_seg(flow, seg);
                            bool local_syn_failure = !sent && !seg->partial_sent && seg->syn && flow->base.state == TCP_SYN_SENT;
                            if (!sent) seg->timer_ms = 0;
                            if (local_syn_failure && seg->local_failures < UINT8_MAX) seg->local_failures++;
                            if (local_syn_failure && seg->local_failures >= TCP_MAX_SYN_SEND_FAILURES) { 
                                retire_flow = true;
                                retire_error = SOCK_ERR_SYS;
                            }
                            if (!retire_flow && (sent || seg->partial_sent || local_syn_failure)) {
                                if (sent || seg->partial_sent) tcp_cc_on_timeout(flow);
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
            }

            if (retire_flow) {
                tcp_flow_note_socket_error(flow, retire_error);
                tcp_free_flow(flow);
            }
            tcp_flow_put(flow);
        }

        msleep(10);
    }

    return 0;
}
