#include "tcp_internal.h"
#include "networking/port_manager.h"
#include "net/internet_layer/ipv4.h"
#include "net/internet_layer/ipv6.h"
#include "net/internet_layer/ipv4_utils.h"
#include "net/internet_layer/ipv6_utils.h"
#include "std/memory.h"
#include "math/rng.h"
#include "syscalls/syscalls.h"

tcp_flow_t tcp_flows[MAX_TCP_FLOWS];

int find_flow(uint16_t local_port, ip_version_t ver, const void *remote_ip, uint16_t remote_port){
    for (int i = 0; i < MAX_TCP_FLOWS; i++){
        tcp_flow_t *f = &tcp_flows[i];

        if (f->state == TCP_STATE_CLOSED) continue;
        if (f->local_port != local_port) continue;

        if (f->state == TCP_LISTEN){
            if (!remote_ip && remote_port == 0) return i;
            continue;
        }

        if (!remote_ip) continue;
        if (f->remote.ver != ver) continue;
        if (f->remote.port != remote_port) continue;
        if (memcmp(f->remote.ip, remote_ip, (size_t)(ver == IP_VER6 ? 16 : 4)) == 0) return i;
    }

    return -1;
}

tcp_data *tcp_get_ctx(uint16_t local_port, ip_version_t ver, const void *remote_ip, uint16_t remote_port){
    int idx = find_flow(local_port, ver, remote_ip, remote_port);

    if (idx < 0) return NULL;
    return &tcp_flows[idx].ctx;
}

static void clear_txq(tcp_flow_t *f){
    for (int i = 0; i < TCP_MAX_TX_SEGS; i++){
        tcp_tx_seg_t *s = &f->txq[i];

        if (s->used && s->buf && s->len) free((void *)s->buf, s->len);

        s->used = 0;
        s->syn = 0;
        s->fin = 0;
        s->rtt_sample = 0;
        s->retransmit_cnt = 0;
        s->seq = 0;
        s->len = 0;
        s->buf = 0;
        s->timer_ms = 0;
        s->timeout_ms = 0;
}
}

static void clear_reass(tcp_flow_t *f){
    for (int i = 0; i < TCP_REASS_MAX_SEGS; i++){
        if (f->reass[i].buf && f->reass[i].end > f->reass[i].seq){
            uint32_t l = f->reass[i].end - f->reass[i].seq;
            free((void *)f->reass[i].buf, l);
        }

        f->reass[i].seq = 0;
        f->reass[i].end = 0;
        f->reass[i].buf = 0;
    }

    f->reass_count = 0;
    f->rcv_buf_used = 0;
}

tcp_flow_t *tcp_alloc_flow(void){
    for (int i = 0; i < MAX_TCP_FLOWS; i++){
        tcp_flow_t *f = &tcp_flows[i];

        if (f->state != TCP_STATE_CLOSED) continue;

        f->fin_pending = 0;
        f->fin_seq = 0;
        f->retries = 0;
        f->snd_wnd = 0;
        f->snd_una = 0;
        f->snd_nxt = 0;
        f->srtt = 0;
        f->rttvar = 0;
        f->rto = TCP_INIT_RTO;
        f->rtt_valid = 0;
        f->time_wait_ms = 0;
        f->fin_wait2_ms = 0;

        f->rcv_nxt = 0;
        f->rcv_buf_used = 0;
        f->rcv_wnd_max = TCP_RECV_WINDOW;
        f->rcv_wnd = TCP_RECV_WINDOW;

        f->mss = TCP_DEFAULT_MSS;
        f->cwnd = f->mss;
        f->ssthresh = TCP_RECV_WINDOW;
        f->dup_acks = 0;
        f->in_fast_recovery = 0;
        f->recover = 0;
        f->cwnd_acc = 0;

        f->persist_active = 0;
        f->persist_timer_ms = 0;
        f->persist_timeout_ms = 0;
        f->persist_probe_cnt = 0;

        f->delayed_ack_pending = 0;
        f->delayed_ack_timer_ms = 0;

        clear_reass(f);
        clear_txq(f);

        return f;
    }

    return NULL;
}

void tcp_free_flow(int idx){
    if (idx < 0 || idx >= MAX_TCP_FLOWS) return;

    tcp_flow_t *f = &tcp_flows[idx];

    clear_txq(f);
    clear_reass(f);

    f->state = TCP_STATE_CLOSED;
    f->local_port = 0;

    f->local.ver = 0;
    memset(f->local.ip, 0, sizeof(f->local.ip));
    f->local.port = 0;

    f->remote.ver = 0;
    memset(f->remote.ip, 0, sizeof(f->remote.ip));
    f->remote.port = 0;

    f->fin_pending = 0;
    f->fin_seq = 0;

    f->ctx.sequence = 0;
    f->ctx.ack = 0;
    f->ctx.flags = 0;
    f->ctx.window = 0;
    f->ctx.options.ptr = 0;
    f->ctx.options.size = 0;
    f->ctx.payload.ptr = 0;
    f->ctx.payload.size = 0;
    f->ctx.expected_ack = 0;
    f->ctx.ack_received = 0;

    f->retries = 0;
    f->snd_wnd = 0;
    f->snd_una = 0;
    f->snd_nxt = 0;

    f->srtt = 0;
    f->rttvar = 0;
    f->rto = TCP_INIT_RTO;
    f->rtt_valid = 0;

    f->time_wait_ms = 0;
    f->fin_wait2_ms = 0;

    f->rcv_nxt = 0;
    f->rcv_buf_used = 0;
    f->rcv_wnd_max = TCP_RECV_WINDOW;
    f->rcv_wnd = TCP_RECV_WINDOW;

    f->mss = TCP_DEFAULT_MSS;

    f->ws_send = 0;
    f->ws_recv = 0;
    f->ws_ok = 0;
    f->sack_ok = 0;

    f->cwnd = f->mss;
    f->ssthresh = TCP_RECV_WINDOW;
    f->dup_acks = 0;
    f->in_fast_recovery = 0;
    f->recover = 0;
    f->cwnd_acc = 0;

    f->persist_active = 0;
    f->persist_timer_ms = 0;
    f->persist_timeout_ms = 0;
    f->persist_probe_cnt = 0;

    f->delayed_ack_pending = 0;
    f->delayed_ack_timer_ms = 0;
}

bool tcp_send_segment(ip_version_t ver, const void *src_ip_addr, const void *dst_ip_addr, tcp_hdr_t *hdr, const uint8_t *opts, uint8_t opts_len, const uint8_t *payload, uint16_t payload_len, const ip_tx_opts_t *txp){
    if (!hdr) return false;

    if (opts_len & 3u) return false;
    if (opts_len > 40u) return false;

    uint16_t tcp_len = (uint16_t)(sizeof(tcp_hdr_t) + opts_len + payload_len);
    uint8_t *segment = (uint8_t *)malloc(tcp_len);
    if (!segment) return false;

    tcp_hdr_t h = *hdr;

    uint8_t header_words = (uint8_t)((sizeof(tcp_hdr_t) + opts_len) / 4);
    h.data_offset_reserved = (uint8_t)(header_words << 4);
    h.window = bswap16(h.window);
    h.checksum = 0;

    memcpy(segment, &h, sizeof(tcp_hdr_t));
    if (opts_len && opts) memcpy(segment + sizeof(tcp_hdr_t), opts, opts_len);
    if (payload_len && payload) memcpy(segment + sizeof(tcp_hdr_t) + opts_len, payload, payload_len);

    if (ver == IP_VER4){
        uint32_t s = *(const uint32_t *)src_ip_addr;
        uint32_t d = *(const uint32_t *)dst_ip_addr;

        ((tcp_hdr_t *)segment)->checksum = tcp_checksum_ipv4(segment, tcp_len, s, d);
        ipv4_send_packet(d, 6, (sizedptr){ (uintptr_t)segment, tcp_len }, (const ipv4_tx_opts_t *)txp, 0);
    } else if (ver == IP_VER6){
        ((tcp_hdr_t *)segment)->checksum = tcp_checksum_ipv6(segment, tcp_len, (const uint8_t *)src_ip_addr, (const uint8_t *)dst_ip_addr);
        ipv6_send_packet((const uint8_t *)dst_ip_addr, 6, (sizedptr){ (uintptr_t)segment, tcp_len }, (const ipv6_tx_opts_t *)txp, 0);
    } else{
        free(segment, tcp_len);
        return false;
    }

    free(segment, tcp_len);
    return true;
}

void tcp_send_reset(ip_version_t ver, const void *src_ip_addr, const void *dst_ip_addr, uint16_t src_port, uint16_t dst_port, uint32_t seq, uint32_t ack, bool ack_valid){
    tcp_hdr_t rst_hdr;

    rst_hdr.src_port = bswap16(src_port);
    rst_hdr.dst_port = bswap16(dst_port);

    if (ack_valid){
        rst_hdr.sequence = bswap32(0);
        rst_hdr.ack = bswap32(ack);
        rst_hdr.flags = (uint8_t)((1u << RST_F) | (1u << ACK_F));
    } else{
        rst_hdr.sequence = bswap32(seq);
        rst_hdr.ack = bswap32(0);
        rst_hdr.flags = (uint8_t)(1u << RST_F);
    }

    rst_hdr.window = 0;
    rst_hdr.urgent_ptr = 0;

    if (ver == IP_VER4){
        ipv4_tx_opts_t tx;

        tcp_build_tx_opts_from_local_v4(src_ip_addr, &tx);
        tcp_send_segment(IP_VER4, src_ip_addr, dst_ip_addr, &rst_hdr, NULL, 0, NULL, 0, (const ip_tx_opts_t *)&tx);
    } else if (ver == IP_VER6){
        ipv6_tx_opts_t tx;

        tcp_build_tx_opts_from_local_v6(src_ip_addr, &tx);
        tcp_send_segment(IP_VER6, src_ip_addr, dst_ip_addr, &rst_hdr, NULL, 0, NULL, 0, (const ip_tx_opts_t *)&tx);
    }
}

void tcp_rtt_update(tcp_flow_t *flow, uint32_t sample_ms){
    if (sample_ms == 0) sample_ms = 1;

    if (!flow->rtt_valid){
        flow->srtt = sample_ms;
        flow->rttvar = sample_ms / 2;

        uint32_t rto = flow->srtt + (flow->rttvar << 2);
        if (rto < TCP_MIN_RTO) rto = TCP_MIN_RTO;
        if (rto > TCP_MAX_RTO) rto = TCP_MAX_RTO;

        flow->rto = rto;
        flow->rtt_valid = 1;

        return;
    }

    uint32_t srtt = flow->srtt;
    uint32_t rttvar = flow->rttvar;

    uint32_t diff = srtt > sample_ms ? srtt - sample_ms : sample_ms - srtt;
    uint32_t new_rttvar = (uint32_t)((3 * (uint64_t)rttvar + (uint64_t)diff) >> 2);
    uint32_t new_srtt = (uint32_t)(((uint64_t)7 * srtt + sample_ms) >> 3);

    flow->srtt = new_srtt;
    flow->rttvar = new_rttvar;

    uint32_t rto = new_srtt + (new_rttvar << 2);
    if (rto < TCP_MIN_RTO) rto = TCP_MIN_RTO;
    if (rto > TCP_MAX_RTO) rto = TCP_MAX_RTO;

    flow->rto = rto;
}

bool tcp_bind_l3(uint8_t l3_id, uint16_t port, uint16_t pid, port_recv_handler_t handler){
    port_manager_t *pm = NULL;

    if (l3_ipv4_find_by_id(l3_id)) pm = ifmgr_pm_v4(l3_id);
    else if (l3_ipv6_find_by_id(l3_id)) pm = ifmgr_pm_v6(l3_id);

    if (!pm) return false;
    if (!port_bind_manual(pm, PROTO_TCP, port, pid, handler)) return false;

    tcp_flow_t *f = tcp_alloc_flow();

    if (f){
        f->local_port = port;

        f->local.ver = 0;
        memset(f->local.ip, 0, sizeof(f->local.ip));
        f->local.port = 0;

        f->remote.ver = 0;
        memset(f->remote.ip, 0, sizeof(f->remote.ip));
        f->remote.port = 0;

        f->state = TCP_LISTEN;

        f->ctx.sequence = 0;
        f->ctx.ack = 0;
        f->ctx.flags = 0;

        f->rcv_wnd_max = TCP_RECV_WINDOW;
        f->rcv_buf_used = 0;
        f->rcv_wnd = TCP_RECV_WINDOW;
        f->ctx.window = TCP_RECV_WINDOW;

        f->mss = TCP_DEFAULT_MSS;
        f->ws_send = 8;
        f->ws_recv = 0;
        f->ws_ok = 1;
        f->sack_ok = 1;

        f->ctx.options.ptr = 0;
        f->ctx.options.size = 0;
        f->ctx.payload.ptr = 0;
        f->ctx.payload.size = 0;

        f->ctx.expected_ack = 0;
        f->ctx.ack_received = 0;

        f->time_wait_ms = 0;
        f->fin_wait2_ms = 0;
    }

    return true;
}

int tcp_alloc_ephemeral_l3(uint8_t l3_id, uint16_t pid, port_recv_handler_t handler){
    port_manager_t *pm = NULL;

    if (l3_ipv4_find_by_id(l3_id)) pm = ifmgr_pm_v4(l3_id);
    else if (l3_ipv6_find_by_id(l3_id)) pm = ifmgr_pm_v6(l3_id);

    if (!pm) return -1;
    return port_alloc_ephemeral(pm, PROTO_TCP, pid, handler);
}

bool tcp_unbind_l3(uint8_t l3_id, uint16_t port, uint16_t pid){
    port_manager_t *pm = NULL;

    if (l3_ipv4_find_by_id(l3_id)) pm = ifmgr_pm_v4(l3_id);
    else if (l3_ipv6_find_by_id(l3_id)) pm = ifmgr_pm_v6(l3_id);

    if (!pm) return false;

    bool res = port_unbind(pm, PROTO_TCP, port, pid);

    if (res){
        for (int i = 0; i < MAX_TCP_FLOWS; i++){
            if (tcp_flows[i].local_port == port && tcp_flows[i].state == TCP_LISTEN) tcp_free_flow(i);
        }
    }

    return res;
}

bool tcp_handshake_l3(uint8_t l3_id, uint16_t local_port, net_l4_endpoint *dst, tcp_data *flow_ctx, uint16_t pid){
    (void)pid;

    tcp_flow_t *flow = tcp_alloc_flow();
    if (!flow) return false;

    int idx = (int)(flow - tcp_flows);

    flow->local_port = local_port;

    flow->remote.ver = dst->ver;
    memcpy(flow->remote.ip, dst->ip, (size_t)(dst->ver == IP_VER6 ? 16 : 4));
    flow->remote.port = dst->port;

    if (dst->ver == IP_VER4){
        l3_ipv4_interface_t *v4 = l3_ipv4_find_by_id(l3_id);

        if (!v4 || !v4->ip){
            tcp_free_flow(idx);
            return false;
        }

        flow->local.ver = IP_VER4;
        memset(flow->local.ip, 0, sizeof(flow->local.ip));
        memcpy(flow->local.ip, &v4->ip, sizeof(v4->ip));
        flow->local.port = local_port;
    } else{
        l3_ipv6_interface_t *v6 = l3_ipv6_find_by_id(l3_id);

        if (!v6 || ipv6_is_unspecified(v6->ip)){
            tcp_free_flow(idx);
            return false;
        }

        flow->local.ver = IP_VER6;
        memset(flow->local.ip, 0, sizeof(flow->local.ip));
        memcpy(flow->local.ip, v6->ip, sizeof(flow->local.ip));
        flow->local.port = local_port;
    }

    flow->state = TCP_SYN_SENT;
    flow->retries = TCP_SYN_RETRIES;

    rng_t rng;
    rng_init_random(&rng);
    uint32_t iss = rng_next32(&rng);

    flow->ctx.sequence = iss;
    flow->ctx.ack = 0;

    flow->rcv_nxt = 0;
    flow->rcv_buf_used = 0;
    flow->rcv_wnd_max = TCP_RECV_WINDOW;
    flow->rcv_wnd = TCP_RECV_WINDOW;

    flow->mss = (flow->local.ver == IP_VER6 ? 1440u : 1460u);

    flow->ws_send = 8;
    flow->ws_recv = 0;
    flow->ws_ok = 0;
    flow->sack_ok = 1;

    uint32_t wnd = (uint32_t)TCP_RECV_WINDOW >> flow->ws_send;
    if (wnd > 0xFFFFu) wnd = 0xFFFFu;
    flow->ctx.window = (uint16_t)wnd;

    flow->ctx.options.ptr = 0;
    flow->ctx.options.size = 0;
    flow->ctx.payload.ptr = 0;
    flow->ctx.payload.size = 0;

    flow->ctx.flags = (uint8_t)(1u << SYN_F);
    flow->ctx.expected_ack = iss + 1;
    flow->ctx.ack_received = 0;

    flow->snd_una = iss;
    flow->snd_nxt = iss;
    flow->snd_wnd = 0;

    flow->cwnd = flow->mss;
    flow->ssthresh = TCP_RECV_WINDOW;
    flow->dup_acks = 0;
    flow->in_fast_recovery = 0;
    flow->recover = 0;
    flow->cwnd_acc = 0;

    flow->time_wait_ms = 0;
    flow->fin_wait2_ms = 0;

    clear_reass(flow);
    clear_txq(flow);

    tcp_tx_seg_t *seg = tcp_alloc_tx_seg(flow);

    if (!seg){
        tcp_free_flow(idx);
        return false;
    }

    seg->syn = 1;
    seg->fin = 0;
    seg->rtt_sample = 1;
    seg->retransmit_cnt = 0;
    seg->seq = flow->snd_nxt;
    seg->len = 0;
    seg->buf = 0;
    seg->timer_ms = 0;
    seg->timeout_ms = flow->rto;

    tcp_hdr_t syn_hdr;

    syn_hdr.src_port = bswap16(local_port);
    syn_hdr.dst_port = bswap16(dst->port);
    syn_hdr.sequence = bswap32(flow->snd_nxt);
    syn_hdr.ack = bswap32(0);
    syn_hdr.flags = (uint8_t)(1u << SYN_F);
    syn_hdr.window = flow->ctx.window;
    syn_hdr.urgent_ptr = 0;

    uint8_t syn_opts[40];
    uint8_t syn_opts_len = tcp_build_syn_options(syn_opts, (uint16_t)flow->mss, flow->ws_send, flow->sack_ok);

    if (dst->ver == IP_VER4){
        ipv4_tx_opts_t tx;

        tcp_build_tx_opts_from_l3(l3_id, &tx);
        tcp_send_segment(IP_VER4, flow->local.ip, flow->remote.ip, &syn_hdr, syn_opts, syn_opts_len, NULL, 0, (const ip_tx_opts_t *)&tx);
    } else{
        ipv6_tx_opts_t tx;

        tx.scope = IP_TX_BOUND_L3;
        tx.index = l3_id;
        tcp_send_segment(IP_VER6, flow->local.ip, flow->remote.ip, &syn_hdr, syn_opts, syn_opts_len, NULL, 0, (const ip_tx_opts_t *)&tx);
    }

    flow->snd_nxt += 1;
    flow->ctx.sequence = flow->snd_nxt;
    flow->ctx.expected_ack = flow->snd_nxt;

    tcp_daemon_kick();

    uint64_t waited = 0;
    const uint64_t interval = 50;
    const uint64_t max_wait = TCP_MAX_RTO;

    while (waited < max_wait){
        if (flow->state == TCP_ESTABLISHED){
            tcp_data *ctx = tcp_get_ctx(local_port, dst->ver, dst->ip, dst->port);
            if (!ctx) return false;

            *flow_ctx = *ctx;
            return true;
        }

        if (flow->state == TCP_STATE_CLOSED){
            tcp_free_flow(idx);
            return false;
        }

        sleep(interval);
        waited += interval;
    }

    tcp_free_flow(idx);
    return false;
}