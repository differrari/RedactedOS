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
uint16_t tcp_active_flows[MAX_TCP_FLOWS];
uint16_t tcp_active_count;
uint32_t tcp_ooo_global_bytes;
uint32_t tcp_ooo_global_segs;
uint32_t tcp_tx_global_bytes;
tcp_stats_t tcp_stats;
static uint32_t tcp_generation_next = 1;

//TODO these statics are small and i find them annoying but theyre good for making the code more readable
//maybe move them to a dedicated file?
uint32_t tcp_clamp_rcvbuf(uint32_t size) {
    if (!size) size = TCP_DEFAULT_RCV_BUF;
    if (size < TCP_RCV_BUF_MIN) size = TCP_RCV_BUF_MIN;
    if (size > TCP_RCV_BUF_MAX) size = TCP_RCV_BUF_MAX;
    return size;
}

void tcp_account_ooo_remove(uint32_t bytes, uint32_t segs) {
    if (tcp_ooo_global_bytes >= bytes) tcp_ooo_global_bytes -= bytes;
    else tcp_ooo_global_bytes = 0;
    if (tcp_ooo_global_segs >= segs) tcp_ooo_global_segs -= segs;
    else tcp_ooo_global_segs = 0;
}

void tcp_account_tx_remove(tcp_flow_t *flow, uint32_t bytes) {
    if (!flow || !bytes) return;
    if (flow->tx.queued_bytes >= bytes) flow->tx.queued_bytes -= bytes;
    else flow->tx.queued_bytes = 0;
    if (tcp_tx_global_bytes >= bytes) tcp_tx_global_bytes -= bytes;
    else tcp_tx_global_bytes = 0;
}

tcp_admit_result_t tcp_admit_ooo(tcp_flow_t *flow, uint32_t increase, uint32_t remaining_nodes) {
    if (!flow) return TCP_ADMIT_OOO_FLOW_BYTES;

    uint32_t mss = flow->tx.mss ? flow->tx.mss : TCP_DEFAULT_MSS;
    uint32_t limit = flow->rx.rcv_wnd_max >> 1;
    uint32_t floor = mss * 4u;

    if (floor > TCP_REASS_MAX_BYTES / 2) floor = TCP_REASS_MAX_BYTES / 2;
    if (limit > TCP_REASS_MAX_BYTES) limit = TCP_REASS_MAX_BYTES;
    if (limit < floor) limit = floor;
    if (limit > flow->rx.rcv_wnd_max) limit = flow->rx.rcv_wnd_max;

    uint32_t seg_increase = remaining_nodes >= flow->rx.reass_count ? 1u : 0;

    if (remaining_nodes >= TCP_REASS_MAX_SEGS) return TCP_ADMIT_OOO_FLOW_SEGS;
    if (increase && flow->rx.rcv_ooo_used + increase > limit) return TCP_ADMIT_OOO_FLOW_BYTES;
    if (increase && tcp_ooo_global_bytes + increase > TCP_REASS_GLOBAL_MAX_BYTES) return TCP_ADMIT_OOO_GLOBAL_BYTES;
    if (seg_increase && tcp_ooo_global_segs + seg_increase > TCP_REASS_GLOBAL_MAX_SEGS) return TCP_ADMIT_OOO_GLOBAL_SEGS;

    return TCP_ADMIT_OK;
}

tcp_admit_result_t tcp_admit_syn(uint8_t l3_id, uint16_t port, ip_version_t ver, const void *src_ip) {
    uint32_t syn_total = 0;
    uint32_t syn_listener = 0;
    uint32_t syn_source = 0;
    uint32_t timewait = 0;
    uint32_t orphan = 0;
    size_t ip_len = (size_t)(ver == IP_VER6 ? 16 : 4);

    for (int i = 0; i < MAX_TCP_FLOWS; i++) {
        tcp_flow_t *f = tcp_flows[i];
        if (!f) continue;

        if (f->base.state == TCP_SYN_RECEIVED) {
            syn_total++;
            if (f->base.local_port == port && f->base.l3_id == l3_id) syn_listener++;
            if (src_ip && f->base.local_port == port && f->base.l3_id == l3_id && f->base.remote.ver == ver && memcmp(f->base.remote.ip, src_ip, ip_len) == 0) syn_source++;
        } else if (f->base.state == TCP_TIME_WAIT) timewait++;
        else if (f->base.state != TCP_LISTEN && f->base.state != TCP_STATE_CLOSED && !f->base.local_port) orphan++;
    }

    uint32_t score = (uint32_t)tcp_active_count * 4 + syn_total * 4 + timewait + orphan * 4 + (tcp_ooo_global_bytes >> 12) + (tcp_tx_global_bytes >> 12);

    if (tcp_active_count >= MAX_TCP_FLOWS) return TCP_ADMIT_FLOW_TABLE_FULL;
    if (syn_total >= TCP_SYN_RECV_MAX_GLOBAL) return TCP_ADMIT_SYN_GLOBAL;
    if (syn_listener >= TCP_SYN_RECV_MAX_LISTENER) return TCP_ADMIT_SYN_LISTENER;
    if (syn_source >= TCP_SYN_RECV_MAX_SOURCE) return TCP_ADMIT_SYN_SOURCE;
    if (orphan >= TCP_ORPHAN_MAX_GLOBAL) return TCP_ADMIT_ORPHAN_LIMIT;
    if (score >= TCP_RESOURCE_BUDGET) return TCP_ADMIT_RESOURCE_BUDGET;

    return TCP_ADMIT_OK;
}

void tcp_enter_time_wait(tcp_flow_t *flow) {
    if (!flow) return;

    uint32_t timewait = 0;
    int oldest = -1;
    uint32_t oldest_ms = 0;

    for (int i = 0; i < MAX_TCP_FLOWS; i++) {
        tcp_flow_t *f = tcp_flows[i];
        if (!f || f->base.state != TCP_TIME_WAIT) continue;
        timewait++;
        if ((oldest < 0 || f->timer.time_wait_ms > oldest_ms) && f != flow) {
            oldest = i;
            oldest_ms = f->timer.time_wait_ms;
        }
    }

    if (timewait >= TCP_TIMEWAIT_MAX_GLOBAL && oldest >= 0) {
        tcp_stats.timewait_reap_oldest++;
        tcp_free_flow(oldest);
    }

    flow->base.state = TCP_TIME_WAIT;
    tcp_release_io_buffers(flow);
    flow->timer.time_wait_ms = 0;
    tcp_daemon_kick();
}

int find_flow(uint16_t local_port, ip_version_t ver, const void *local_ip, const void *remote_ip, uint16_t remote_port){
    for (uint16_t n = 0; n < tcp_active_count; n++){
        int i = tcp_active_flows[n];
        tcp_flow_t *f = tcp_flows[i];
        if (!f) continue;

        if (f->base.state == TCP_STATE_CLOSED) continue;
        if (f->base.local_port != local_port) continue;

        if (f->base.state == TCP_LISTEN){
            if (remote_ip || remote_port) continue;
            if (f->base.local.ver && f->base.local.ver != ver) continue;
            if (!local_ip) return i;

            size_t l = (size_t)(ver == IP_VER6 ? 16 : 4);
            int unspec = 1;
            for (size_t k = 0; k < l; ++k){
                if (f->base.local.ip[k]){
                    unspec = 0;
                    break;
                }
            }
            if (unspec) return i;
            if (memcmp(f->base.local.ip, local_ip, l) == 0) return i;
            continue;
        }

        if (!remote_ip) continue;
        if (!local_ip) continue;
        if (f->base.remote.ver != ver) continue;
        if (f->base.remote.port != remote_port) continue;

        size_t l = (size_t)(ver == IP_VER6 ? 16 : 4);
        if (memcmp(f->base.local.ip, local_ip, l) != 0) continue;
        if (memcmp(f->base.remote.ip, remote_ip, l) != 0) continue;

        return i;
    }

    return -1;
}

bool tcp_get_ctx(uint16_t local_port, ip_version_t ver, const void *local_ip, const void *remote_ip, uint16_t remote_port, tcp_data *out_ctx){
    if (!out_ctx) return false;
    int idx = find_flow(local_port, ver, local_ip, remote_ip, remote_port);
    if (idx < 0) return false;
    *out_ctx = tcp_flows[idx]->base.ctx;
    return true;
}

tcp_flow_t *tcp_flow_from_ctx(tcp_data *flow_ctx) {
    if (!flow_ctx) return NULL;
    if (flow_ctx->flow_index >= MAX_TCP_FLOWS) return NULL;

    tcp_flow_t *flow = tcp_flows[flow_ctx->flow_index];
    if (!flow) return NULL;
    if (flow->base.generation != flow_ctx->flow_generation) return NULL;
    return flow;
}

void tcp_release_io_buffers(tcp_flow_t *f) {
    if (!f) return;

    for (int i = 0; i < TCP_MAX_TX_SEGS; i++) {
        tcp_tx_seg_t *s = &f->tx.txq[i];
        if (s->used && s->buf && s->len) {
            tcp_account_tx_remove(f, (uint32_t)s->len);
            release((void*)s->buf);
        }
        memset(s, 0, sizeof(*s));
    }

    if (f->tx.nagle_buf) release((void*)f->tx.nagle_buf);
    f->tx.nagle_buf = 0;
    f->tx.nagle_len = 0;
    f->tx.nagle_cap = 0;
    f->tx.nagle_timer_ms = 0;
    f->tx.nagle_flushing = 0;
    f->tx.nagle_appending = 0;

    if (f->rx.reass_count || f->rx.rcv_ooo_used) tcp_account_ooo_remove(f->rx.rcv_ooo_used, f->rx.reass_count);
    if (f->rx.rcv_buf) release((void*)f->rx.rcv_buf);
    f->rx.rcv_buf = 0;
    f->rx.rcv_base = f->rx.rcv_nxt;
    f->rx.rcv_data_nxt = f->rx.rcv_nxt;
    f->rx.rcv_ooo_used = 0;
    f->rx.rcv_wnd = 0;
    f->rx.rcv_adv_edge = f->rx.rcv_nxt;
    f->rx.fin_pending = 0;
    f->base.ctx.window = 0;
    f->rx.reass_count = 0;
    memset(f->rx.reass, 0, sizeof(f->rx.reass));
}

tcp_flow_t *tcp_alloc_flow(void){
    for (int i = 0; i < MAX_TCP_FLOWS; i++){
        if (tcp_flows[i]) continue;

        tcp_flow_t *f = (tcp_flow_t *)zalloc(sizeof(tcp_flow_t));
        if (!f) return NULL;
        tcp_flows[i] = f;

        if (tcp_active_count >= MAX_TCP_FLOWS) {
            release(f);
            tcp_flows[i] = NULL;
            return NULL;
        }
        f->base.active_pos = tcp_active_count;
        tcp_active_flows[tcp_active_count++] = (uint16_t)i;
        f->base.slot = (uint16_t)i;
        f->base.generation = tcp_generation_next++;
        if (!tcp_generation_next) tcp_generation_next = 1;
        f->base.ctx.flow_index = f->base.slot;
        f->base.ctx.flow_generation = f->base.generation;

        f->tx.rto = TCP_INIT_RTO;
        f->rx.rcv_wnd_max = tcp_clamp_rcvbuf(TCP_DEFAULT_RCV_BUF);
        f->rx.rcv_wnd = f->rx.rcv_wnd_max;

        f->tx.mss = TCP_DEFAULT_MSS;
        f->tx.cwnd = f->tx.mss * TCP_INIT_CWND_SEGS;
        f->tx.ssthresh = TCP_RECV_WINDOW;
        return f;
    }

    return NULL;
}

void tcp_free_flow(int idx) {
    if (idx < 0 || idx >= MAX_TCP_FLOWS) return;

    tcp_flow_t *f = tcp_flows[idx];
    if (!f) return;

    if (tcp_active_count != 0) {
        uint16_t pos = f->base.active_pos;

        if (pos >= tcp_active_count || tcp_active_flows[pos] != idx) for (pos = 0; pos < tcp_active_count && tcp_active_flows[pos] != idx; pos++);
        if (pos < tcp_active_count) {
            uint16_t last = tcp_active_flows[--tcp_active_count];
            tcp_active_flows[pos] = last;
            tcp_active_flows[tcp_active_count] = 0;

            if (last != idx && tcp_flows[last]) tcp_flows[last]->base.active_pos = pos;
        }

        f->base.active_pos = 0;
    }

    for (int i = 0; i < TCP_MAX_TX_SEGS; i++) {
        tcp_tx_seg_t *s =&f->tx.txq[i];
        if (s->used && s->buf && s->len) {
            tcp_account_tx_remove(f, (uint32_t)s->len);
            release((void*)s->buf);
        }
        memset(s, 0, sizeof(*s));
    }

    if (f->tx.nagle_buf) release((void*)f->tx.nagle_buf);
    f->tx.nagle_buf = 0;
    f->tx.nagle_len = 0;
    f->tx.nagle_cap = 0;
    f->tx.nagle_timer_ms = 0;
    f->tx.nagle_flushing = 0;
    f->tx.nagle_appending = 0;

    if (f->rx.reass_count || f->rx.rcv_ooo_used) tcp_account_ooo_remove(f->rx.rcv_ooo_used, f->rx.reass_count);
    if (f->rx.rcv_buf) release((void*)f->rx.rcv_buf);
    f->rx.rcv_buf = 0;
    f->rx.rcv_base = f->rx.rcv_nxt;
    f->rx.rcv_data_nxt = f->rx.rcv_nxt;
    f->rx.rcv_ooo_used = 0;
    f->rx.rcv_wnd = 0;
    f->rx.rcv_adv_edge = f->rx.rcv_nxt;
    f->rx.fin_pending = 0;
    f->base.ctx.window = 0;
    f->rx.reass_count = 0;

    memset(f->rx.reass, 0, sizeof(f->rx.reass));
    memset(f, 0, sizeof(*f));
    release(f);
    tcp_flows[idx] = NULL;
}

bool tcp_flow_is_closed(tcp_data *flow_ctx) {
    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow) return true;
    return flow->base.state == TCP_STATE_CLOSED || flow->base.state == TCP_TIME_WAIT;
}

tcp_result_t tcp_flow_release_closed(tcp_data *flow_ctx) {
    tcp_flow_t *flow = tcp_flow_from_ctx(flow_ctx);
    if (!flow) return TCP_INVALID;
    if (flow->base.state != TCP_STATE_CLOSED && flow->base.state != TCP_TIME_WAIT) return TCP_BUSY;
    tcp_free_flow((int)flow->base.slot);
    return TCP_OK;
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
        uint32_t s = 0;
        uint32_t d = 0;
        memcpy(&s, src_ip_addr, sizeof(s));
        memcpy(&d, dst_ip_addr, sizeof(d));

        h.checksum = tcp_checksum_ipv4(segment, tcp_len, s, d);
        memcpy(segment, &h, sizeof(h));
        return ipv4_send_packet(d, 6, pkt, (const ipv4_tx_opts_t *)txp, ttl, dontfrag);
    } else if (ver == IP_VER6){
        h.checksum = tcp_checksum_ipv6(segment, tcp_len, (const uint8_t *)src_ip_addr, (const uint8_t *)dst_ip_addr);
        memcpy(segment, &h, sizeof(h));
        return ipv6_send_packet((const uint8_t *)dst_ip_addr, 6, pkt, (const ipv6_tx_opts_t *)txp, ttl, dontfrag);
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

    if (!flow->tx.rtt_valid){
        flow->tx.srtt = sample_ms;
        flow->tx.rttvar = sample_ms / 2;

        uint32_t rto = flow->tx.srtt + (flow->tx.rttvar << 2);
        if (rto < TCP_MIN_RTO) rto = TCP_MIN_RTO;
        if (rto > TCP_MAX_RTO) rto = TCP_MAX_RTO;

        flow->tx.rto = rto;
        flow->tx.rtt_valid = 1;

        return;
    }

    uint32_t srtt = flow->tx.srtt;
    uint32_t rttvar = flow->tx.rttvar;

    uint32_t diff = srtt > sample_ms ? srtt - sample_ms : sample_ms - srtt;
    uint32_t new_rttvar = (uint32_t)((3 * (uint64_t)rttvar + (uint64_t)diff) >> 2);
    uint32_t new_srtt = (uint32_t)(((uint64_t)7 * srtt + sample_ms) >> 3);

    flow->tx.srtt = new_srtt;
    flow->tx.rttvar = new_rttvar;

    uint32_t rto = new_srtt + (new_rttvar << 2);
    if (rto < TCP_MIN_RTO) rto = TCP_MIN_RTO;
    if (rto > TCP_MAX_RTO) rto = TCP_MAX_RTO;

    flow->tx.rto = rto;
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
        f->base.local_port = port;
        f->base.l3_id = l3_id;

        f->base.local.ver = l3_is_v6_from_id(l3_id) ? IP_VER6 : IP_VER4;
        memset(f->base.local.ip, 0, sizeof(f->base.local.ip));
        f->base.local.port = port;

        f->base.remote.ver = 0;
        memset(f->base.remote.ip, 0, sizeof(f->base.remote.ip));
        f->base.remote.port = 0;

        f->base.state = TCP_LISTEN;

        f->base.ctx.sequence = 0;
        f->base.ctx.ack = 0;
        f->base.ctx.flags = 0;

        f->rx.rcv_wnd_max = tcp_clamp_rcvbuf(TCP_DEFAULT_RCV_BUF);
        if (extra && (extra->flags & SOCK_OPT_BUF_SIZE) && extra->buf_size) f->rx.rcv_wnd_max = tcp_clamp_rcvbuf(extra->buf_size);
        f->rx.rcv_base = 0;
        f->rx.rcv_data_nxt = 0;
        f->rx.rcv_ooo_used = 0;
        f->rx.sack_recent_left = 0;
        f->rx.sack_recent_right = 0;
        f->rx.rcv_adv_edge = 0;

        f->ip.ttl = extra && (extra->flags & SOCK_OPT_TTL) ? extra->ttl : 0;
        f->ip.dontfrag = extra && (extra->flags & SOCK_OPT_DONTFRAG) ? 1 : 0;
        f->timer.keepalive_on = extra && (extra->flags & SOCK_OPT_KEEPALIVE) ? 1 : 0;
        f->timer.keepalive_ms = extra && (extra->flags & SOCK_OPT_KEEPALIVE) ? extra->keepalive_ms : 0;
        f->timer.keepalive_idle_ms = 0;

        f->tx.mss = TCP_DEFAULT_MSS;
        if (f->rx.rcv_wnd_max > 65535u) {
            f->tx.ws_send = 8;
            f->tx.ws_recv = 0;
            f->tx.ws_ok = 1;
        } else {
            f->tx.ws_send = 0;
            f->tx.ws_recv = 0;
            f->tx.ws_ok = 0;
        }
        f->tx.sack_ok = 1;


        f->base.ctx.options.ptr = 0;
        f->base.ctx.options.size = 0;
        f->base.ctx.payload.ptr = 0;
        f->base.ctx.payload.size = 0;

        f->base.ctx.expected_ack = 0;
        f->base.ctx.ack_received = 0;

        f->timer.time_wait_ms = 0;
        f->timer.fin_wait2_ms = 0;
    }

    return true;
}

int tcp_alloc_ephemeral_l3(uint8_t l3_id, uint16_t pid, port_recv_handler_t handler){

    port_manager_t *pm = l3_is_v6_from_id(l3_id) ? ifmgr_pm_v6(l3_id) : ifmgr_pm_v4(l3_id);
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
            if (f->base.state==TCP_LISTEN && f->base.local_port==port && f->base.local.ver==ver) tcp_free_flow(i);
        }
    }

    return res;
}

bool tcp_handshake_l3(uint8_t l3_id, uint16_t local_port, net_l4_endpoint *dst, tcp_data *flow_ctx, uint16_t pid, const SocketExtraOptions* extra){
    (void)pid;
    if (!dst || !flow_ctx) return false;

    tcp_flow_t *flow = tcp_alloc_flow();
    if (!flow) return false;

    int idx = (int)flow->base.slot;

    flow->base.local_port = local_port;
    flow->base.l3_id = l3_id;

    flow->base.remote.ver = dst->ver;
    memcpy(flow->base.remote.ip, dst->ip, (size_t)(dst->ver == IP_VER6 ? 16 : 4));
    flow->base.remote.port = dst->port;

    if (dst->ver == IP_VER4){
        l3_ipv4_interface_t *v4 = l3_ipv4_find_by_id(l3_id);

        if (!v4 || !v4->ip){
            tcp_free_flow(idx);
            return false;
        }

        make_ep(v4->ip, local_port, IP_VER4, &flow->base.local);
    } else{
        l3_ipv6_interface_t *v6 = l3_ipv6_find_by_id(l3_id);

        if (!v6 || ipv6_is_unspecified(v6->ip)){
            tcp_free_flow(idx);
            return false;
        }

        flow->base.local.ver = IP_VER6;
        memset(flow->base.local.ip, 0, sizeof(flow->base.local.ip));
        memcpy(flow->base.local.ip, v6->ip, sizeof(flow->base.local.ip));
        flow->base.local.port = local_port;
    }

    flow->base.state = TCP_SYN_SENT;
    flow->base.retries = TCP_SYN_RETRIES;

    rng_t rng;
    uint64_t virt_timer;
    asm volatile ("mrs %0, cntvct_el0" : "=r"(virt_timer));
    rng_seed(&rng, virt_timer);
    uint32_t iss = rng_next32(&rng);

    flow->base.ctx.sequence = iss;
    flow->base.ctx.ack = 0;

    flow->rx.rcv_nxt = 0;
    flow->rx.rcv_base = 0;
    flow->rx.rcv_data_nxt = 0;
    flow->rx.rcv_ooo_used = 0;
    flow->rx.sack_recent_left = 0;
    flow->rx.sack_recent_right = 0;
    flow->rx.rcv_wnd_max = tcp_clamp_rcvbuf(TCP_DEFAULT_RCV_BUF);
    if (extra && (extra->flags & SOCK_OPT_BUF_SIZE) && extra->buf_size) flow->rx.rcv_wnd_max = tcp_clamp_rcvbuf(extra->buf_size);
    flow->rx.rcv_adv_edge = 0;

    flow->tx.mss = tcp_calc_mss_for_l3(l3_id, dst->ver, dst->ip);

    if (flow->rx.rcv_wnd_max > 65535u) {
        flow->tx.ws_send = 8;
        flow->tx.ws_recv = 0;
        flow->tx.ws_ok = 1;
    } else {
        flow->tx.ws_send = 0;
        flow->tx.ws_recv = 0;
        flow->tx.ws_ok = 0;
    }
    flow->tx.sack_ok = 1;
    flow->rx.rcv_buf = (uintptr_t)zalloc(flow->rx.rcv_wnd_max);
    if (!flow->rx.rcv_buf) {
        flow->rx.rcv_wnd = 0;
        flow->rx.rcv_adv_edge = flow->rx.rcv_nxt;
        flow->base.ctx.window = 0;
        tcp_free_flow(idx);
        return false;
    }
    (void)tcp_calc_adv_wnd_field(flow, 1);

    flow->ip.ttl = extra && (extra->flags & SOCK_OPT_TTL) ? extra->ttl : 0;
    flow->ip.dontfrag = extra && (extra->flags & SOCK_OPT_DONTFRAG) ? 1 : 0;
    flow->timer.keepalive_on = extra && (extra->flags & SOCK_OPT_KEEPALIVE) ? 1 : 0;
    flow->timer.keepalive_ms = extra && (extra->flags & SOCK_OPT_KEEPALIVE) ? extra->keepalive_ms : 0;
    flow->timer.keepalive_idle_ms = 0;

    flow->base.ctx.options.ptr = 0;
    flow->base.ctx.options.size = 0;
    flow->base.ctx.payload.ptr = 0;
    flow->base.ctx.payload.size = 0;

    flow->base.ctx.flags = (uint8_t)(1u << SYN_F);
    flow->base.ctx.expected_ack = iss + 1;
    flow->base.ctx.ack_received = 0;

    flow->tx.snd_una = iss;
    flow->tx.snd_nxt = iss;
    flow->tx.snd_wnd = 0;

    flow->tx.cwnd = flow->tx.mss * TCP_INIT_CWND_SEGS;
    flow->tx.ssthresh = TCP_RECV_WINDOW;
    flow->tx.dup_acks = 0;
    flow->tx.in_fast_recovery = 0;
    flow->tx.recover = 0;
    flow->tx.cwnd_acc = 0;

    flow->timer.time_wait_ms = 0;
    flow->timer.fin_wait2_ms = 0;

    tcp_tx_seg_t *seg = tcp_alloc_tx_seg(flow);

    if (!seg){
        tcp_free_flow(idx);
        return false;
    }

    seg->syn = 1;
    seg->fin = 0;
    seg->rtt_sample = 1;
    seg->retransmit_cnt = 0;
    seg->seq = flow->tx.snd_nxt;
    seg->len = 0;
    seg->buf = 0;
    seg->timer_ms = 0;
    seg->timeout_ms = flow->tx.rto ? flow->tx.rto : TCP_INIT_RTO;

    seg->opts_len = tcp_build_syn_options(seg->opts, (uint16_t)flow->tx.mss, flow->rx.rcv_wnd_max > 65535u ? flow->tx.ws_send : 0xffu, flow->tx.sack_ok);

    if (!tcp_send_from_seg(flow, seg)) {
        tcp_free_flow(idx);
        return false;
    }

    flow->tx.snd_nxt += 1;
    flow->base.ctx.sequence = flow->tx.snd_nxt;
    flow->base.ctx.expected_ack = flow->tx.snd_nxt;

    tcp_daemon_kick();

    uint64_t waited = 0;
    const uint64_t interval = 50;
    const uint64_t max_wait = TCP_CONNECT_TIMEOUT_MS;

    while (waited < max_wait){
        if (flow->base.state == TCP_ESTABLISHED || flow->base.state == TCP_CLOSE_WAIT){
            *flow_ctx = flow->base.ctx;
            return true;
        }

        if (flow->base.state == TCP_STATE_CLOSED){
            tcp_free_flow(idx);
            return false;
        }

        msleep(interval);
        waited += interval;
    }

    tcp_free_flow(idx);
    return false;
}