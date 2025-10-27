#include "tcp.h"
#include "types.h"
#include "networking/port_manager.h"
#include "net/internet_layer/ipv4.h"
#include "std/memory.h"
#include "math/rng.h"
#include "syscalls/syscalls.h"
#include "net/internet_layer/ipv4_utils.h"
//TODO: add mtu check and fragmentation. also fragment rebuild


static tcp_flow_t tcp_flows[MAX_TCP_FLOWS];

static inline int ip_len(ip_version_t ver){ return ver==IP_VER6 ? 16 : 4; }
static inline uint32_t v4_u32_from_ptr(const void *p){ return *(const uint32_t*)p; }

tcp_data* tcp_get_ctx(uint16_t local_port, ip_version_t ver, const void *remote_ip, uint16_t remote_port){
    int idx = find_flow(local_port, ver, remote_ip, remote_port);
    return (idx < 0) ? NULL : &tcp_flows[idx].ctx;
}

static uint32_t checksum_add(uint32_t sum, uint16_t val){
    sum += val;
    if (sum > 0xFFFF) sum = (sum & 0xFFFF) + 1;
    return sum;
}

static uint16_t tcp_compute_checksum_v4(const void *segment, uint16_t seg_len, uint32_t src_ip, uint32_t dst_ip){
    const uint8_t *seg = (const uint8_t*)segment;
    const uint64_t total_len = 12 + seg_len;

    uintptr_t raw = (uintptr_t)malloc(total_len);
    if (!raw) return 0;

    uint8_t *buf = (uint8_t *)raw;

    buf[0] = (src_ip >> 24) & 0xFF;
    buf[1] = (src_ip >> 16) & 0xFF;
    buf[2] = (src_ip >> 8) & 0xFF;
    buf[3] = (src_ip >> 0) & 0xFF;
    buf[4] = (dst_ip >> 24) & 0xFF;
    buf[5] = (dst_ip >> 16) & 0xFF;
    buf[6] = (dst_ip >> 8) & 0xFF;
    buf[7] = (dst_ip >> 0) & 0xFF;
    buf[8] = 0;
    buf[9] = 6;
    buf[10] = (seg_len >> 8) & 0xFF;
    buf[11] = (seg_len >> 0) & 0xFF;
    memcpy(buf + 12, seg, seg_len);
    buf[12 + 16] = 0;
    buf[12 + 17] = 0;

    uint32_t sum = 0;
    for (uint64_t i = 0; i + 1 < total_len; i += 2) {
        uint16_t word = (uint16_t)buf[i] << 8 | buf[i + 1];
        sum = checksum_add(sum, word);
    }
    if (total_len & 1) {
        uint16_t word = (uint16_t)buf[total_len - 1] << 8;
        sum = checksum_add(sum, word);
    }

    uint16_t res = (uint16_t)(~sum & 0xFFFF);
    free((void *)raw, total_len);
    return bswap16(res);
}

static uint16_t tcp_compute_checksum_v6(const void*, uint16_t, const uint8_t[16], const uint8_t[16]) {
    //TODO IPV6
    return 0;
}

int find_flow(uint16_t local_port, ip_version_t ver, const void *remote_ip, uint16_t remote_port) {
    for (int i = 0; i < MAX_TCP_FLOWS; ++i) {
        tcp_flow_t *f = &tcp_flows[i];
        if (f->state == TCP_STATE_CLOSED) continue;
        if (f->local_port != local_port) continue;

        if (f->state == TCP_LISTEN){
            if (!remote_ip && remote_port == 0) return i;
            continue;
        }
        if (f->remote.ver == ver && f->remote.port == remote_port && remote_ip) {
            if (memcmp(f->remote.ip, remote_ip, (uint64_t)ip_len(ver)) == 0) return i;
        }
    }
    return -1;
}

static int allocate_flow_entry() {
    for (int i = 0; i < MAX_TCP_FLOWS; ++i) {
        if (tcp_flows[i].state == TCP_STATE_CLOSED) {
            tcp_flows[i].retries = 0;
            return i;
        }
    }
    return -1;
}

static void free_flow_entry(int idx) {
    if (idx < 0 || idx >= MAX_TCP_FLOWS) return;
    tcp_flow_t *f = &tcp_flows[idx];

    f->state = TCP_STATE_CLOSED;
    f->local_port = 0;

    f->local.ver = 0;
    memset(f->local.ip, 0, 16);
    f->local.port = 0;
    f->remote.ver = 0;
    memset(f->remote.ip, 0, 16);
    f->remote.port = 0;

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
}

static inline port_manager_t* pm_for_l3(uint8_t l3_id){
    if (l3_ipv4_find_by_id(l3_id)) return ifmgr_pm_v4(l3_id);
    if (l3_ipv6_find_by_id(l3_id)) return ifmgr_pm_v6(l3_id);
    return NULL;
}

static bool build_tx_opts_from_local_v4(const void* src_ip_addr, ipv4_tx_opts_t* out){
    if (!out) return false;
    uint32_t lip = v4_u32_from_ptr(src_ip_addr);
    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_ip(lip);
    if (v4){
        out->scope = IP_TX_BOUND_L3;
        out->index = v4->l3_id;
    } else {
        out->scope = IP_TX_AUTO;
        out->index = 0;
    }
    return true;
}

static bool build_tx_opts_from_l3(uint8_t l3_id, ipv4_tx_opts_t* out){
    if (!out) return false;
    out->scope = IP_TX_BOUND_L3;
    out->index = l3_id;
    return true;
}

static bool send_tcp_segment(ip_version_t ver, const void *src_ip_addr, const void *dst_ip_addr, tcp_hdr_t *hdr, const uint8_t *payload, uint16_t payload_len, const ipv4_tx_opts_t* txp){
    uint8_t header_words = sizeof(tcp_hdr_t) / 4;
    hdr->data_offset_reserved = (uint8_t)((header_words << 4) | 0x0);
    hdr->window = bswap16(hdr->window);

    uint16_t tcp_len = (uint16_t)(sizeof(tcp_hdr_t) + payload_len);
    uint8_t *segment = (uint8_t*) malloc(tcp_len);
    if (!segment) return false;

    memcpy(segment, hdr, sizeof(tcp_hdr_t));
    if (payload_len) memcpy(segment + sizeof(tcp_hdr_t), payload, payload_len);
    tcp_hdr_t *hdr_on_buf = (tcp_hdr_t*)segment;
    hdr_on_buf->checksum = 0;

    if (ver == IP_VER4){
        uint32_t s = v4_u32_from_ptr(src_ip_addr);
        uint32_t d = v4_u32_from_ptr(dst_ip_addr);
        hdr_on_buf->checksum = tcp_compute_checksum_v4(segment, tcp_len, s, d);
        ipv4_send_packet(d, 6, (sizedptr){ (uintptr_t)segment, tcp_len }, txp, 0);
    } else {
        free(segment, tcp_len);
        return false;
    }

    free(segment, tcp_len);
    return true;
}

static void send_reset(ip_version_t ver, const void *src_ip_addr, const void *dst_ip_addr,
                       uint16_t src_port, uint16_t dst_port, uint32_t seq, uint32_t ack, bool ack_valid) {
    tcp_hdr_t rst_hdr;
    rst_hdr.src_port = bswap16(src_port);
    rst_hdr.dst_port = bswap16(dst_port);
    if (ack_valid){
        rst_hdr.sequence = bswap32(0);
        rst_hdr.ack      = bswap32(seq + 1);
        rst_hdr.flags    = (1 << RST_F) | (1 << ACK_F);
    } else {
        rst_hdr.sequence = bswap32(ack);
        rst_hdr.ack      = bswap32(0);
        rst_hdr.flags    = (1 << RST_F);
    }
    rst_hdr.window = 0;
    rst_hdr.urgent_ptr = 0;
    ipv4_tx_opts_t tx; build_tx_opts_from_local_v4(src_ip_addr, &tx);
    send_tcp_segment(ver, src_ip_addr, dst_ip_addr, &rst_hdr, NULL, 0, (ver==IP_VER4? &tx : NULL));
}

bool tcp_bind_l3(uint8_t l3_id, uint16_t port, uint16_t pid, port_recv_handler_t handler) {
    port_manager_t* pm = pm_for_l3(l3_id);
    if (!pm) return false;
    if (!port_bind_manual(pm, PROTO_TCP, port, pid, handler)) return false;

    int idx = allocate_flow_entry();
    if (idx >= 0){
        tcp_flow_t *f = &tcp_flows[idx];
        f->local_port = port;

        f->local.ver = 0;
        memset(f->local.ip,  0, 16);
        f->local.port  = 0;
        f->remote.ver = 0;
        memset(f->remote.ip, 0, 16);
        f->remote.port = 0;
        f->state = TCP_LISTEN;
        f->ctx.sequence = 0;
        f->ctx.ack = 0;
        f->ctx.flags = 0;
        f->ctx.window = 0xFFFF;
        f->ctx.options.ptr = 0;
        f->ctx.options.size = 0;
        f->ctx.payload.ptr = 0;
        f->ctx.payload.size = 0;
        f->ctx.expected_ack = 0;
        f->ctx.ack_received = 0;
        f->retries = 0;
    }
    return true;
}

int tcp_alloc_ephemeral_l3(uint8_t l3_id, uint16_t pid, port_recv_handler_t handler) {
    port_manager_t* pm = pm_for_l3(l3_id);
    if (!pm) return -1;
    return port_alloc_ephemeral(pm, PROTO_TCP, pid, handler);
}

bool tcp_unbind_l3(uint8_t l3_id, uint16_t port, uint16_t pid) {
    port_manager_t* pm = pm_for_l3(l3_id);
    if (!pm) return false;

    bool res = port_unbind(pm, PROTO_TCP, port, pid);
    if (res) {
        for (int i = 0; i < MAX_TCP_FLOWS; ++i) {
            if (tcp_flows[i].local_port == port) {
                if (tcp_flows[i].state == TCP_LISTEN) free_flow_entry(i);
            }
        }
    }
    return res;
}

bool tcp_handshake_l3(uint8_t l3_id, uint16_t local_port, net_l4_endpoint *dst, tcp_data *flow_ctx, uint16_t pid) {
    int idx = allocate_flow_entry();
    if (idx < 0) return false;

    tcp_flow_t *flow = &tcp_flows[idx];
    flow->local_port = local_port;

    flow->remote.ver = dst->ver;
    memcpy(flow->remote.ip, dst->ip, (uint64_t)ip_len(dst->ver));
    flow->remote.port = dst->port;

    if (dst->ver == IP_VER4){
        l3_ipv4_interface_t *v4 = l3_ipv4_find_by_id(l3_id);
        if (!v4 || !v4->ip) { free_flow_entry(idx); return false; }
        flow->local.ver = IP_VER4;
        memset(flow->local.ip, 0, 16);
        memcpy(flow->local.ip, &v4->ip, 4);
        flow->local.port = local_port;
    } else {
        memset(flow->local.ip, 0, 16);
        flow->local.ver = IP_VER6;
        flow->local.port = local_port;
    }

    flow->state = TCP_SYN_SENT;
    flow->retries = TCP_SYN_RETRIES;

    const uint32_t iss = 1;
    flow->ctx.sequence = iss;
    flow->ctx.ack = 0;
    flow->ctx.window = 0xFFFF;
    flow->ctx.options.ptr = 0;
    flow->ctx.options.size = 0;
    flow->ctx.payload.ptr = 0;
    flow->ctx.payload.size = 0;
    flow->ctx.flags = (uint8_t)(1 << SYN_F);
    flow->ctx.expected_ack = iss + 1;
    flow->ctx.ack_received = 0;

    tcp_hdr_t syn_hdr;
    syn_hdr.src_port = bswap16(local_port);
    syn_hdr.dst_port = bswap16(dst->port);
    syn_hdr.sequence = bswap32(flow->ctx.sequence);
    syn_hdr.ack = bswap32(0);
    syn_hdr.flags = (uint8_t)(1<<SYN_F);
    syn_hdr.window = flow->ctx.window;
    syn_hdr.urgent_ptr = 0;

    bool sent = false;
    if (dst->ver == IP_VER4) {
        ipv4_tx_opts_t tx; build_tx_opts_from_l3(l3_id, &tx);
        while (flow->retries-- > 0) {
            sent = send_tcp_segment(IP_VER4, flow->local.ip, flow->remote.ip, &syn_hdr, NULL, 0, &tx);
            if (!sent) {
                break;
            }
            uint64_t wait_ms = TCP_RETRY_TIMEOUT_MS, elapsed = 0;
            const uint64_t interval = 50;
            while (elapsed < wait_ms) {
                if (flow->state == TCP_ESTABLISHED) {
                    *flow_ctx = flow->ctx;
                    return true;
                }
                if (flow->state == TCP_STATE_CLOSED) {
                    free_flow_entry(idx);
                    return false;
                }
                sleep(interval);
                elapsed += interval;
            }
        }
    } else {
        //TODO IPV6
    }

    free_flow_entry(idx);
    return false;
}

tcp_result_t tcp_flow_send(tcp_data *flow_ctx) {
    if (!flow_ctx) return TCP_INVALID;

    tcp_flow_t *flow = NULL;
    for (int i = 0; i < MAX_TCP_FLOWS; ++i) {
        if (&tcp_flows[i].ctx == flow_ctx) { flow = &tcp_flows[i]; break; }
    }
    if (!flow) return TCP_INVALID;

    const uint8_t flags = flow_ctx->flags;
    uint8_t *payload_ptr = (uint8_t*) flow_ctx->payload.ptr;
    uint16_t payload_len = flow_ctx->payload.size;
    if (flow->state != TCP_ESTABLISHED && !(flags & (1<<FIN_F))) {
        if (!(flow->state == TCP_CLOSE_WAIT && (flags & (1<<FIN_F)))) return TCP_INVALID;
    }

    tcp_hdr_t hdr;
    hdr.src_port = bswap16(flow->local_port);
    hdr.dst_port = bswap16(flow->remote.port);
    hdr.sequence = bswap32(flow_ctx->sequence);
    hdr.ack = bswap32(flow_ctx->ack);
    hdr.flags = flags;
    hdr.window = flow_ctx->window ? flow_ctx->window : 0xFFFF;
    hdr.urgent_ptr = 0;

    bool sent = false;
    if (flow->remote.ver == IP_VER4) {
        ipv4_tx_opts_t tx; build_tx_opts_from_local_v4(flow->local.ip, &tx);
        sent = send_tcp_segment(IP_VER4, flow->local.ip, flow->remote.ip, &hdr, payload_ptr, payload_len, &tx);
    } else {
        sent = false; //TODO IPV6
    }
    if (!sent) return TCP_RESET;

    uint32_t seq_incr = payload_len;
    if (flags & (1<<SYN_F)) seq_incr += 1;
    if (flags & (1<<FIN_F)) seq_incr += 1;
    flow_ctx->sequence += seq_incr;

    if ((flags & (1<<FIN_F)) || payload_len > 0) {
        flow_ctx->expected_ack = flow_ctx->sequence;

        int retries = TCP_DATA_RETRIES;
        while (retries-- > 0) {
            uint64_t wait_ms = TCP_RETRY_TIMEOUT_MS;
            uint64_t elapsed = 0;
            const uint64_t interval = 50;
            while (elapsed < wait_ms) {
                if (flow_ctx->ack_received >= flow_ctx->expected_ack) break;
                if (flow->state == TCP_STATE_CLOSED) return TCP_RESET;
                sleep(interval);
                elapsed += interval;
            }
            if (flow_ctx->ack_received >= flow_ctx->expected_ack) break;
            if (flow->state >= TCP_CLOSING || flow->state == TCP_STATE_CLOSED) break;

            flow_ctx->sequence -= seq_incr;
            if (flow->remote.ver == IP_VER4) {
                ipv4_tx_opts_t tx; build_tx_opts_from_local_v4(flow->local.ip, &tx);
                send_tcp_segment(IP_VER4, flow->local.ip, flow->remote.ip, &hdr, payload_ptr, payload_len, &tx);
            }
            flow_ctx->sequence += seq_incr;
        }
        if (flow_ctx->ack_received < flow_ctx->expected_ack) {
            return TCP_TIMEOUT;
        }
    }
    return TCP_OK;
}

tcp_result_t tcp_flow_close(tcp_data *flow_ctx) {
    if (!flow_ctx) return TCP_INVALID;

    tcp_flow_t *flow = NULL;
    for (int i = 0; i < MAX_TCP_FLOWS; ++i) {
        if (&tcp_flows[i].ctx == flow_ctx) { flow = &tcp_flows[i]; break; }
    }
    if (!flow) return TCP_INVALID;

    if (flow->state == TCP_ESTABLISHED || flow->state == TCP_CLOSE_WAIT) {
        flow_ctx->sequence = flow->ctx.sequence;
        flow_ctx->ack = flow->ctx.ack;
        flow_ctx->window = flow->ctx.window ? flow->ctx.window : 0xFFFF;

        flow_ctx->payload.ptr  = 0;
        flow_ctx->payload.size = 0;
        flow_ctx->flags = (uint8_t)((1u << FIN_F) | (1u << ACK_F));

        tcp_result_t res = tcp_flow_send(flow_ctx);
        if (res != TCP_OK)  return res;

        if (flow->state == TCP_ESTABLISHED) flow->state = TCP_FIN_WAIT_1;
        else flow->state = TCP_LAST_ACK;

        const uint64_t max_wait = 2000;
        const uint64_t interval = 100;
        uint64_t elapsed = 0;
        while (elapsed < max_wait) {
            if (flow->state == TCP_STATE_CLOSED) break;
            sleep(interval);
            elapsed += interval;
        }

        int idx = (int)(flow - tcp_flows);
        free_flow_entry(idx);
        return TCP_OK;
    }
    return TCP_INVALID;
}

void tcp_input(ip_version_t ipver, const void *src_ip_addr, const void *dst_ip_addr, uint8_t l3_id, uintptr_t ptr, uint32_t len) {
    if (len < sizeof(tcp_hdr_t)) return;

    tcp_hdr_t *hdr = (tcp_hdr_t*) ptr;

    if (ipver == IP_VER4){
        uint16_t recv_checksum = hdr->checksum;
        hdr->checksum = 0;
        uint16_t calc = tcp_compute_checksum_v4((uint8_t*)hdr, (uint16_t)len, v4_u32_from_ptr(src_ip_addr), v4_u32_from_ptr(dst_ip_addr));
        hdr->checksum = recv_checksum;
        if (recv_checksum != calc) return;
    } else {
        //TODO IPV6
    }

    uint16_t src_port = bswap16(hdr->src_port);
    uint16_t dst_port = bswap16(hdr->dst_port);
    uint32_t seq = bswap32(hdr->sequence);
    uint32_t ack = bswap32(hdr->ack);
    uint8_t flags = hdr->flags;
    uint16_t window = bswap16(hdr->window);

    int idx = find_flow(dst_port, ipver, src_ip_addr, src_port);
    tcp_flow_t *flow = (idx >= 0 ? &tcp_flows[idx] : NULL);
    if (flow) flow->ctx.window = window;

    l3_ipv4_interface_t *v4 = l3_ipv4_find_by_id(l3_id);
    l3_ipv6_interface_t *v6 = v4 ? NULL : l3_ipv6_find_by_id(l3_id);
    port_manager_t *pm = v4 ? ifmgr_pm_v4(l3_id) : (v6 ? ifmgr_pm_v6(l3_id) : NULL);
    if (!pm) return;

    uint8_t ifx = v4 ? (v4->l2 ? v4->l2->ifindex : 0) : (v6 && v6->l2 ? v6->l2->ifindex : 0);

    if (!flow) {
        int listen_idx = find_flow(dst_port, IP_VER4, NULL, 0);
        if ((flags & (1u<<SYN_F)) && !(flags & (1u<<ACK_F)) && listen_idx >= 0) {
            rng_t rng;
            rng_init_random(&rng);

            tcp_flow_t *lf = &tcp_flows[listen_idx];
            int new_idx = allocate_flow_entry();
            if (new_idx < 0) return;

            flow = &tcp_flows[new_idx];
            flow->local_port = dst_port;

            flow->remote.ver = ipver;
            memset(flow->remote.ip, 0, 16);
            memcpy(flow->remote.ip, src_ip_addr, (uint64_t)ip_len(ipver));
            flow->remote.port = src_port;

            flow->local.ver = ipver;
            memset(flow->local.ip, 0, 16);
            if (ipver == IP_VER4 && v4) memcpy(flow->local.ip, &v4->ip, 4);
            flow->local.port = dst_port;

            flow->state = TCP_SYN_RECEIVED;
            flow->retries = TCP_SYN_RETRIES;

            flow->ctx.window = lf->ctx.window ? lf->ctx.window : 0xFFFF;
            flow->ctx.flags = 0;
            flow->ctx.options = lf->ctx.options;
            flow->ctx.payload.ptr  = 0;
            flow->ctx.payload.size = 0;

            uint32_t iss = rng_next32(&rng);
            flow->ctx.sequence = iss;
            flow->ctx.ack = seq + 1;
            flow->ctx.expected_ack = iss + 1;
            flow->ctx.ack_received = 0;

            tcp_hdr_t synack_hdr;
            synack_hdr.src_port = bswap16(dst_port);
            synack_hdr.dst_port = bswap16(src_port);
            synack_hdr.sequence = bswap32(iss);
            synack_hdr.ack      = bswap32(seq + 1);
            synack_hdr.flags    = (uint8_t)((1u<<SYN_F)|(1u<<ACK_F));
            synack_hdr.window   = flow->ctx.window;
            synack_hdr.urgent_ptr = 0;

            if (ipver == IP_VER4 && v4) {
                ipv4_tx_opts_t tx; build_tx_opts_from_l3(l3_id, &tx);
                send_tcp_segment(IP_VER4, flow->local.ip, src_ip_addr, &synack_hdr, NULL, 0, &tx);
            }
            return;
        } else {
            if (!(flags & (1u<<RST_F))) {
                if (flags & (1u<<ACK_F)) {
                    send_reset(ipver, dst_ip_addr, src_ip_addr, dst_port, src_port, seq, ack, false);
                } else {
                    send_reset(ipver, dst_ip_addr, src_ip_addr, dst_port, src_port, seq, ack, true);
                }
            }
            return;
        }
    }

    switch (flow->state) {
    case TCP_SYN_SENT:
        if ((flags & (1<<SYN_F)) && (flags & (1<<ACK_F))) {
            if (ack == flow->ctx.expected_ack) {
                flow->ctx.ack = seq + 1;
                flow->ctx.ack_received = ack;
                flow->ctx.sequence += 1;

                tcp_hdr_t final_ack;
                final_ack.src_port = bswap16(flow->local_port);
                final_ack.dst_port = bswap16(flow->remote.port);
                final_ack.sequence = bswap32(flow->ctx.sequence);
                final_ack.ack      = bswap32(flow->ctx.ack);
                final_ack.flags    = (1<<ACK_F);
                final_ack.window   = flow->ctx.window;
                final_ack.urgent_ptr = 0;

                if (flow->remote.ver == IP_VER4){
                    ipv4_tx_opts_t tx; build_tx_opts_from_local_v4(flow->local.ip, &tx);
                    (void)send_tcp_segment(IP_VER4, flow->local.ip, flow->remote.ip, &final_ack, NULL, 0, &tx);
                }
                flow->state = TCP_ESTABLISHED;
            }
        } else if (flags & (1<<RST_F)) {
            flow->state = TCP_STATE_CLOSED;
        }
        return;
    case TCP_SYN_RECEIVED:
        if ((flags & (1<<ACK_F)) && !(flags & (1<<SYN_F)) && !(flags & (1<<RST_F))) {
            if (ack == flow->ctx.expected_ack) {
                flow->ctx.sequence += 1;
                flow->state = TCP_ESTABLISHED;
                flow->ctx.ack_received = ack;

                if (pm){
                    port_recv_handler_t h = port_get_handler(pm, PROTO_TCP, dst_port);
                    if (h) h(ifx, ipver, src_ip_addr, dst_ip_addr, 0, 0, src_port, dst_port);
                }
            }
        } else if (flags & (1<<RST_F)){
            free_flow_entry(idx);
        }
        return;

    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
    case TCP_CLOSE_WAIT:
    case TCP_CLOSING:
    case TCP_LAST_ACK: {
        if (flags & (1 << RST_F)) {
            free_flow_entry(idx);
            return;
        }

        uint8_t hdr_len = (uint8_t)((hdr->data_offset_reserved >> 4) * 4);
        if (len < hdr_len) return;
        uint32_t data_len = len - hdr_len;

        if ((flags & (1u << ACK_F)) && ack > flow->ctx.ack_received) {
            flow->ctx.ack_received = ack;
            if (flow->state == TCP_FIN_WAIT_1 && ack == flow->ctx.expected_ack){
                flow->state = TCP_FIN_WAIT_2;
            } else if ((flow->state == TCP_LAST_ACK || flow->state == TCP_CLOSING) && ack == flow->ctx.expected_ack) {
                free_flow_entry(idx);
                return;
            }
        }

        uint32_t rcv_next_old = flow->ctx.ack;
        uint32_t rcv_next_new = rcv_next_old;

        bool data_inseq = (data_len > 0) && (seq == rcv_next_old);
        if (data_inseq) {
            rcv_next_new += data_len;
            if (pm){
                port_recv_handler_t h = port_get_handler(pm, PROTO_TCP, dst_port);
                if (h) h(ifx, ipver, src_ip_addr, dst_ip_addr, ptr + hdr_len, data_len, src_port, dst_port);
            }
        }

        bool fin_set = (flags & (1<<FIN_F)) != 0;
        bool fin_inseq = fin_set && ((seq + data_len) == rcv_next_new);
        if (fin_inseq) rcv_next_new += 1;

        if (rcv_next_new != rcv_next_old){
            flow->ctx.ack = rcv_next_new;

            tcp_hdr_t ackhdr = {
                .src_port = bswap16(flow->local_port),
                .dst_port = bswap16(flow->remote.port),
                .sequence = bswap32(flow->ctx.sequence),
                .ack = bswap32(flow->ctx.ack),
                .flags = (uint8_t)(1<<ACK_F),
                .window = flow->ctx.window ? flow->ctx.window : 0xFFFF,
                .urgent_ptr = 0
            };
            if (flow->remote.ver == IP_VER4){
                ipv4_tx_opts_t tx; build_tx_opts_from_local_v4(flow->local.ip, &tx);
                (void)send_tcp_segment(IP_VER4, flow->local.ip, flow->remote.ip, &ackhdr, NULL, 0, &tx);
            }
        }

        if (fin_inseq){
            tcp_state_t old = flow->state;
            if (old == TCP_ESTABLISHED) flow->state = TCP_CLOSE_WAIT;
            else if (old == TCP_FIN_WAIT_1) flow->state = TCP_CLOSING;
            else if (old == TCP_FIN_WAIT_2) flow->state = TCP_TIME_WAIT;
            else if (old == TCP_CLOSING) flow->state = TCP_TIME_WAIT;
            else if (old == TCP_LAST_ACK) flow->state = TCP_TIME_WAIT;
        }
        return;
    }

    default:
        break;
    }
}
