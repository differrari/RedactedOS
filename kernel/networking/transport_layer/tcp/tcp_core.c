#include "tcp_internal.h"
#include "networking/port_manager.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "std/memory.h"
#include "math/rng.h"
#include "syscalls/syscalls.h"
#include "networking/transport_layer/trans_utils.h"

tcp_flow_t *tcp_flows[MAX_TCP_FLOWS];


int find_flow(uint16_t local_port, ip_version_t ver, const void *local_ip, const void *remote_ip, uint16_t remote_port){
    for (int i = 0; i < MAX_TCP_FLOWS; i++){
        tcp_flow_t *f = tcp_flows[i];
        if (!f) continue;

        if (f->state == TCP_STATE_CLOSED) continue;
        if (f->local_port != local_port) continue;

        if (f->state == TCP_LISTEN){
            if (remote_ip || remote_port) continue;
            if (f->local.ver && f->local.ver != ver) continue;
            if (!local_ip) return i;

            size_t l = (size_t)(ver == IP_VER6 ? 16 : 4);
            int unspec = 1;
            for (size_t k = 0; k < l; ++k){
                if (f->local.ip[k]){
                    unspec = 0;
                    break;
                }
            }
            if (unspec) return i;
            if (memcmp(f->local.ip, local_ip, l) == 0) return i;
            continue;
        }

        if (!remote_ip) continue;
        if (!local_ip) continue;
        if (f->remote.ver != ver) continue;
        if (f->remote.port != remote_port) continue;

        size_t l = (size_t)(ver == IP_VER6 ? 16 : 4);
        if (memcmp(f->local.ip, local_ip, l) != 0) continue;
        if (memcmp(f->remote.ip, remote_ip, l) != 0) continue;

        return i;
    }

    return -1;
}

tcp_data *tcp_get_ctx(uint16_t local_port, ip_version_t ver, const void *local_ip, const void *remote_ip, uint16_t remote_port){
    int idx = find_flow(local_port, ver, local_ip, remote_ip, remote_port);

    if (idx < 0) return NULL;
    return &tcp_flows[idx]->ctx;
}

static void clear_txq(tcp_flow_t *f){
    for (int i = 0; i < TCP_MAX_TX_SEGS; i++){
        tcp_tx_seg_t *s = &f->txq[i];

        if (s->used && s->buf && s->len) free_sized((void *)s->buf, s->len);

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
            free_sized((void *)f->reass[i].buf, l);
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
        if (tcp_flows[i]) continue;

        tcp_flow_t *f = (tcp_flow_t *)malloc(sizeof(tcp_flow_t));
        if (!f) return NULL;
        memset(f, 0, sizeof(tcp_flow_t));
        tcp_flows[i] = f;

        f->rto = TCP_INIT_RTO;
        f->rcv_wnd_max = TCP_DEFAULT_RCV_BUF;
        f->rcv_wnd = f->rcv_wnd_max;

        f->mss = TCP_DEFAULT_MSS;
        f->cwnd = f->mss;
        f->ssthresh = TCP_RECV_WINDOW;

        clear_reass(f);
        clear_txq(f);

        return f;
    }

    return NULL;
}

void tcp_free_flow(int idx) {
    if (idx < 0 || idx >= MAX_TCP_FLOWS) return;

    tcp_flow_t *f = tcp_flows[idx];
    if (!f) return;

    clear_txq(f);
    clear_reass(f);

    memset(f, 0, sizeof(*f));

    f->state = TCP_STATE_CLOSED;

    f->rto = TCP_INIT_RTO;

    f->rcv_wnd_max = TCP_DEFAULT_RCV_BUF;
    f->rcv_wnd = f->rcv_wnd_max;

    f->mss = TCP_DEFAULT_MSS;
    f->cwnd = f->mss;
    f->ssthresh = TCP_RECV_WINDOW;

    free_sized(f, sizeof(*f));
    tcp_flows[idx] = NULL;
}

bool tcp_send_segment(ip_version_t ver, const void *src_ip_addr, const void *dst_ip_addr, tcp_hdr_t *hdr, const uint8_t *opts, uint8_t opts_len, const uint8_t *payload, uint16_t payload_len, const ip_tx_opts_t *txp, uint8_t ttl, uint8_t dontfrag){
    if (!hdr) return false;

    if (opts_len & 3u) return false;
    if (opts_len > 40u) return false;

    uint16_t tcp_len = (uint16_t)(sizeof(tcp_hdr_t) + opts_len + payload_len);
    uint32_t headroom = (uint32_t)sizeof(eth_hdr_t) + (uint32_t)(ver == IP_VER4 ? sizeof(ipv4_hdr_t) : sizeof(ipv6_hdr_t));
    netpkt_t *pkt = netpkt_alloc(tcp_len, headroom, 0);
    if (!pkt) return false;
    uint8_t *segment = (uint8_t*)netpkt_put(pkt, tcp_len);
    if (!segment) {
        netpkt_unref(pkt);
        return false;
    }

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
        ipv4_send_packet(d, 6, pkt, (const ipv4_tx_opts_t *)txp, ttl, dontfrag);
        return true;
    } else if (ver == IP_VER6){
        ((tcp_hdr_t *)segment)->checksum = tcp_checksum_ipv6(segment, tcp_len, (const uint8_t *)src_ip_addr, (const uint8_t *)dst_ip_addr);
        ipv6_send_packet((const uint8_t *)dst_ip_addr, 6, pkt, (const ipv6_tx_opts_t *)txp, ttl, dontfrag);
        return true;
    }

    netpkt_unref(pkt);
    return false;
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
        tcp_send_segment(IP_VER4, src_ip_addr, dst_ip_addr, &rst_hdr, NULL, 0, NULL, 0, (const ip_tx_opts_t *)&tx, 0, 0);
    } else if (ver == IP_VER6){
        ipv6_tx_opts_t tx;

        tcp_build_tx_opts_from_local_v6(src_ip_addr, &tx);
        tcp_send_segment(IP_VER6, src_ip_addr, dst_ip_addr, &rst_hdr, NULL, 0, NULL, 0, (const ip_tx_opts_t *)&tx, 0, 0);
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

bool tcp_bind_l3(uint8_t l3_id, uint16_t port, uint16_t pid, port_recv_handler_t handler, const SocketExtraOptions* extra){
    ip_version_t ver = l3_is_v6_from_id(l3_id) ? IP_VER6 : IP_VER4;

    port_manager_t *pm = (ver == IP_VER6) ? ifmgr_pm_v6(l3_id) : ifmgr_pm_v4(l3_id);

    if (!pm) return false;
    if (!port_bind_manual(pm, PROTO_TCP, port, pid, handler)) return false;

    int listen_idx = find_flow(port, ver, NULL, NULL, 0);
    if (listen_idx >= 0) return true;

    tcp_flow_t *f = tcp_alloc_flow();
    if (!f) {
        (void)port_unbind(pm, PROTO_TCP, port, pid);
        return false;
    }
    if (f){
        f->local_port = port;
        f->l3_id = l3_id;

        f->local.ver = l3_is_v6_from_id(l3_id) ? IP_VER6 : IP_VER4;
        memset(f->local.ip, 0, sizeof(f->local.ip));
        f->local.port = port;

        f->remote.ver = 0;
        memset(f->remote.ip, 0, sizeof(f->remote.ip));
        f->remote.port = 0;

        f->state = TCP_LISTEN;

        f->ctx.sequence = 0;
        f->ctx.ack = 0;
        f->ctx.flags = 0;

        f->rcv_wnd_max = TCP_DEFAULT_RCV_BUF;
        if (extra && (extra->flags & SOCK_OPT_BUF_SIZE) && extra->buf_size) f->rcv_wnd_max = extra->buf_size;
        f->rcv_buf_used = 0;
        f->rcv_adv_edge = 0;

        f->ip_ttl = extra && (extra->flags & SOCK_OPT_TTL) ? extra->ttl : 0;
        f->ip_dontfrag = extra && (extra->flags & SOCK_OPT_DONTFRAG) ? 1 : 0;
        f->keepalive_on = extra && (extra->flags & SOCK_OPT_KEEPALIVE) ? 1 : 0;
        f->keepalive_ms = extra && (extra->flags & SOCK_OPT_KEEPALIVE) ? extra->keepalive_ms : 0;
        f->keepalive_idle_ms = 0;

        f->mss = TCP_DEFAULT_MSS;
                if (f->rcv_wnd_max > 65535u) {
            f->ws_send = 8;
            f->ws_recv = 0;
            f->ws_ok = 1;
        } else {
            f->ws_send = 0;
            f->ws_recv = 0;
            f->ws_ok = 0;
        }
        f->sack_ok = 1;

        (void)tcp_calc_adv_wnd_field(f, 1);

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

    port_manager_t *pm = l3_is_v6_from_id(l3_id) ? ifmgr_pm_v6(l3_id) : ifmgr_pm_v4(l3_id);
    if (!pm) return -1;

    if (!pm) return -1;
    return port_alloc_ephemeral(pm, PROTO_TCP, pid, handler);
}

bool tcp_unbind_l3(uint8_t l3_id, uint16_t port, uint16_t pid){
    ip_version_t ver = l3_is_v6_from_id(l3_id) ? IP_VER6 : IP_VER4;

    port_manager_t *pm = (ver == IP_VER6) ? ifmgr_pm_v6(l3_id) : ifmgr_pm_v4(l3_id);
    if (!pm) return false;

    bool res = port_unbind(pm, PROTO_TCP, port, pid);

    if (res){
        for (int i = 0; i < MAX_TCP_FLOWS; i++){
            tcp_flow_t *f = tcp_flows[i];
            if (!f) continue;
            if (f->state==TCP_LISTEN && f->local_port==port && f->local.ver==ver) tcp_free_flow(i);
        }
    }

    return res;
}

bool tcp_handshake_l3(uint8_t l3_id, uint16_t local_port, net_l4_endpoint *dst, tcp_data *flow_ctx, uint16_t pid, const SocketExtraOptions* extra){
    (void)pid;

    tcp_flow_t *flow = tcp_alloc_flow();
    if (!flow) return false;

    int idx = -1;
    for (int i = 0; i < MAX_TCP_FLOWS; i++) {
        if (tcp_flows[i] == flow) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return false;

    flow->local_port = local_port;
    flow->l3_id = l3_id;

    flow->remote.ver = dst->ver;
    memcpy(flow->remote.ip, dst->ip, (size_t)(dst->ver == IP_VER6 ? 16 : 4));
    flow->remote.port = dst->port;

    if (dst->ver == IP_VER4){
        l3_ipv4_interface_t *v4 = l3_ipv4_find_by_id(l3_id);

        if (!v4 || !v4->ip){
            tcp_free_flow(idx);
            return false;
        }

        make_ep(v4->ip, local_port, IP_VER4, &flow->local);
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
    uint64_t virt_timer;
    asm volatile ("mrs %0, cntvct_el0" : "=r"(virt_timer));
    rng_seed(&rng, virt_timer);
    uint32_t iss = rng_next32(&rng);

    flow->ctx.sequence = iss;
    flow->ctx.ack = 0;

    flow->rcv_nxt = 0;
    flow->rcv_buf_used = 0;
    flow->rcv_wnd_max = TCP_DEFAULT_RCV_BUF;
    if (extra && (extra->flags & SOCK_OPT_BUF_SIZE) && extra->buf_size) flow->rcv_wnd_max = extra->buf_size;
    flow->rcv_adv_edge = 0;

    flow->mss = tcp_calc_mss_for_l3(l3_id, dst->ver, dst->ip);

    if (flow->rcv_wnd_max > 65535u) {
        flow->ws_send = 8;
        flow->ws_recv = 0;
        flow->ws_ok = 1;
    } else {
        flow->ws_send = 0;
        flow->ws_recv = 0;
        flow->ws_ok = 0;
    }
    flow->sack_ok = 1;

    (void)tcp_calc_adv_wnd_field(flow, 1);

    flow->ip_ttl = extra && (extra->flags & SOCK_OPT_TTL) ? extra->ttl : 0;
    flow->ip_dontfrag = extra && (extra->flags & SOCK_OPT_DONTFRAG) ? 1 : 0;
    flow->keepalive_on = extra && (extra->flags & SOCK_OPT_KEEPALIVE) ? 1 : 0;
    flow->keepalive_ms = extra && (extra->flags & SOCK_OPT_KEEPALIVE) ? extra->keepalive_ms : 0;
    flow->keepalive_idle_ms = 0;

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
    seg->timeout_ms = flow->rto ? flow->rto : TCP_INIT_RTO;

    tcp_hdr_t syn_hdr;

    syn_hdr.src_port = bswap16(local_port);
    syn_hdr.dst_port = bswap16(dst->port);
    syn_hdr.sequence = bswap32(flow->snd_nxt);
    syn_hdr.ack = bswap32(0);
    syn_hdr.flags = (uint8_t)(1u << SYN_F);
    syn_hdr.window = flow->ctx.window;
    syn_hdr.urgent_ptr = 0;

    uint8_t syn_opts[40];
    uint8_t syn_opts_len = tcp_build_syn_options(syn_opts, (uint16_t)flow->mss, flow->rcv_wnd_max > 65535u ? flow->ws_send : 0xffu, flow->sack_ok);

    if (dst->ver == IP_VER4){
        ipv4_tx_opts_t tx;

        tcp_build_tx_opts_from_l3(l3_id, &tx);
        tcp_send_segment(IP_VER4, flow->local.ip, flow->remote.ip, &syn_hdr, syn_opts, syn_opts_len, NULL, 0, (const ip_tx_opts_t *)&tx, flow->ip_ttl, flow->ip_dontfrag);
    } else{
        ipv6_tx_opts_t tx;

        tx.scope = IP_TX_BOUND_L3;
        tx.index = l3_id;
        tcp_send_segment(IP_VER6, flow->local.ip, flow->remote.ip, &syn_hdr, syn_opts, syn_opts_len, NULL, 0, (const ip_tx_opts_t *)&tx, flow->ip_ttl, flow->ip_dontfrag);
    }

    flow->snd_nxt += 1;
    flow->ctx.sequence = flow->snd_nxt;
    flow->ctx.expected_ack = flow->snd_nxt;

    tcp_daemon_kick();

    uint64_t waited = 0;
    const uint64_t interval = 50;
    const uint64_t max_wait = (uint64_t)TCP_MAX_RTO * (uint64_t)(TCP_SYN_RETRIES + 1);

    while (waited < max_wait){
        if (flow->state == TCP_ESTABLISHED){
            tcp_data *ctx = tcp_get_ctx(local_port, dst->ver, flow->local.ip, dst->ip, dst->port);
            if (!ctx) return false;

            *flow_ctx = *ctx;
            return true;
        }

        if (flow->state == TCP_STATE_CLOSED){
            tcp_free_flow(idx);
            return false;
        }

        msleep(interval);
        waited += interval;
    }

    tcp_free_flow(idx);
    return false;
}