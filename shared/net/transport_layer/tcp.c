#include "tcp.h"
#include "types.h"
#include "networking/port_manager.h"
#include "net/internet_layer/ipv4.h"
#include "std/memfunctions.h"
#include "math/rng.h"
//TODO: add mtu check and fragmentation. also fragment rebuild
extern uintptr_t malloc(uint64_t size);
extern void      free(void *ptr, uint64_t size);
extern void      sleep(uint64_t ms);

static tcp_flow_t tcp_flows[MAX_TCP_FLOWS];
static inline uint16_t htons(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}
static inline uint16_t ntohs(uint16_t x) {
    return htons(x);
}

static inline uint32_t htonl(uint32_t x) {
    return ((x & 0x000000FFU) << 24) |
           ((x & 0x0000FF00U) <<  8) |
           ((x & 0x00FF0000U) >>  8) |
           ((x & 0xFF000000U) >> 24);
}
static inline uint32_t ntohl(uint32_t x) {
    return htonl(x);
}
tcp_data* tcp_get_ctx(uint16_t local_port,
                    uint32_t remote_ip,
                    uint16_t remote_port)
{
    int idx = find_flow(local_port, remote_ip, remote_port);
    if (idx < 0)
        return NULL;
    return &tcp_flows[idx].ctx;
}
static uint32_t checksum_add(uint32_t sum, uint16_t val) {
    sum += val;
    if (sum > 0xFFFF) {
        sum = (sum & 0xFFFF) + 1;
    }
    return sum;
}
uint16_t tcp_compute_checksum(const void *segment,
                              uint16_t seg_len,
                              uint32_t src_ip,
                              uint32_t dst_ip)
{
    const uint8_t *seg = (const uint8_t *)segment;
    const uint64_t total_len = 12 + seg_len;

    uintptr_t raw = malloc(total_len);
    if (!raw) {
        return 0;
    }
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

    free((void *)raw, total_len);

    return htons((uint16_t)(~sum & 0xFFFF));
}

static int find_flow(uint16_t local_port, uint32_t remote_ip, uint16_t remote_port) {
    for (int i = 0; i < MAX_TCP_FLOWS; ++i) {
        tcp_flow_t *f = &tcp_flows[i];
        if (f->state != TCP_STATE_CLOSED) {
            if (f->local_port == local_port) {
                if (f->state == TCP_LISTEN) {
                    if (remote_ip == 0 && remote_port == 0) {
                        return i;
                    }
                }
                if (f->remote.ip == remote_ip && f->remote.port == remote_port) {
                    return i;
                }
            }
        }
    }
    return -1;
}

static int allocate_flow_entry() {
    for (int i = 0; i < MAX_TCP_FLOWS; ++i) {
        if (tcp_flows[i].state == TCP_STATE_CLOSED) {
            tcp_flows[i].state = TCP_STATE_CLOSED;
            tcp_flows[i].retries = 0;
            return i;
        }
    }
    return -1;
}

static void free_flow_entry(int idx) {
    if (idx >= 0 && idx < MAX_TCP_FLOWS) {
        tcp_flows[idx].state = TCP_STATE_CLOSED;
        tcp_flows[idx].local_port = 0;
        tcp_flows[idx].remote.ip = 0;
        tcp_flows[idx].remote.port = 0;
        tcp_flows[idx].ctx.sequence = 0;
        tcp_flows[idx].ctx.ack = 0;
        tcp_flows[idx].ctx.flags = 0;
        tcp_flows[idx].ctx.window = 0;
        tcp_flows[idx].ctx.options.ptr = 0;
        tcp_flows[idx].ctx.options.size = 0;
        tcp_flows[idx].ctx.payload.ptr = 0;
        tcp_flows[idx].ctx.payload.size = 0;
        tcp_flows[idx].ctx.expected_ack = 0;
        tcp_flows[idx].ctx.ack_received = 0;
        tcp_flows[idx].retries = 0;
    }
}

static bool send_tcp_segment(uint32_t src_ip, uint32_t dst_ip, tcp_hdr_t *hdr, const uint8_t *payload, uint16_t payload_len) {
    uint8_t header_words = sizeof(tcp_hdr_t) / 4;
    hdr->data_offset_reserved = (header_words << 4) | 0x0;
    hdr->window   = htons(hdr->window);
    uint16_t tcp_len = sizeof(tcp_hdr_t) + payload_len;
    uint8_t *segment = (uint8_t*) malloc(tcp_len);
    if (!segment) {
        return false;
    }
    memcpy(segment, hdr, sizeof(tcp_hdr_t));
    if (payload_len > 0) {
        memcpy(segment + sizeof(tcp_hdr_t), payload, payload_len);
    }
    tcp_hdr_t *hdr_on_buf = (tcp_hdr_t*) segment;
    hdr_on_buf->checksum = 0;
    uint16_t csum = tcp_compute_checksum(segment, tcp_len, src_ip, dst_ip);
    hdr_on_buf->checksum = csum;
    ipv4_send_segment(src_ip, dst_ip, 6, (sizedptr){ .ptr = (uintptr_t)segment, .size = tcp_len });
    free(segment, tcp_len);
    return true;
}

static void send_reset(uint32_t src_ip, uint32_t dst_ip,
                       uint16_t src_port, uint16_t dst_port,
                       uint32_t seq, uint32_t ack, bool ack_valid) {
    tcp_hdr_t rst_hdr;
    rst_hdr.src_port = htons(src_port);
    rst_hdr.dst_port = htons(dst_port);
    if (ack_valid) {
        rst_hdr.sequence = htonl(0);
        rst_hdr.ack      = htonl(seq + 1);
        rst_hdr.flags    = (1 << RST_F) | (1 << ACK_F);
    } else {
        rst_hdr.sequence = htonl(ack);
        rst_hdr.ack      = htonl(0);
        rst_hdr.flags    = (1 << RST_F);
    }
    rst_hdr.window = 0;
    rst_hdr.urgent_ptr = 0;

    send_tcp_segment(src_ip, dst_ip, &rst_hdr, NULL, 0);
}

bool tcp_bind(uint16_t port, uint16_t pid, port_recv_handler_t handler) {
    if (!port_bind_manual(PROTO_TCP, port, pid, handler)) {
        return false;
    }

    int idx = allocate_flow_entry();
    if (idx >= 0) {
        tcp_flows[idx].local_port = port;
        tcp_flows[idx].remote.ip = 0;
        tcp_flows[idx].remote.port = 0;
        tcp_flows[idx].state = TCP_LISTEN;
        tcp_flows[idx].ctx.sequence = 0;
        tcp_flows[idx].ctx.ack = 0;
        tcp_flows[idx].ctx.flags = 0;
        tcp_flows[idx].ctx.window = 0xFFFF;
        tcp_flows[idx].ctx.options.ptr = 0;
        tcp_flows[idx].ctx.options.size = 0;
        tcp_flows[idx].ctx.payload.ptr = 0;
        tcp_flows[idx].ctx.payload.size = 0;
        tcp_flows[idx].ctx.expected_ack = 0;
        tcp_flows[idx].ctx.ack_received = 0;
        tcp_flows[idx].retries = 0;
    }
    return true;
}

int tcp_alloc_ephemeral(uint16_t pid, port_recv_handler_t handler) {
    int port = port_alloc_ephemeral(PROTO_TCP, pid, handler);
    return port;
}

bool tcp_unbind(uint16_t port, uint16_t pid) {
    bool res = port_unbind(PROTO_TCP, port, pid);
    if (res) {
        for (int i = 0; i < MAX_TCP_FLOWS; ++i) {
            if (tcp_flows[i].local_port == port) {
                free_flow_entry(i);
            }
        }
    }
    return res;
}

bool tcp_handshake(uint16_t local_port, net_l4_endpoint *dst, tcp_data *flow_ctx, uint16_t pid) {
    int idx = allocate_flow_entry();
    if (idx < 0) {
        return false;
    }
    tcp_flow_t *flow = &tcp_flows[idx];
    flow->local_port = local_port;
    flow->remote.ip = dst->ip;
    flow->remote.port = dst->port;
    flow->state = TCP_SYN_SENT;
    flow->retries = TCP_SYN_RETRIES;
    uint32_t iss = 1;
    flow->ctx.sequence = iss;
    flow->ctx.ack = 0;
    flow->ctx.window = 0xFFFF;
    flow->ctx.options.ptr = 0;
    flow->ctx.options.size = 0;
    flow->ctx.payload.ptr = 0;
    flow->ctx.payload.size = 0;
    flow->ctx.flags = (1 << SYN_F);
    flow->ctx.expected_ack = iss + 1;
    flow->ctx.ack_received = 0;
    tcp_hdr_t syn_hdr;
    syn_hdr.src_port = htons(local_port);
    syn_hdr.dst_port = htons(dst->port);
    syn_hdr.sequence = htonl(flow->ctx.sequence);
    syn_hdr.ack = htonl(0);
    syn_hdr.flags = (1 << SYN_F);
    syn_hdr.window = flow->ctx.window;
    syn_hdr.urgent_ptr = 0;
    uint32_t src_ip = ipv4_get_cfg()->ip;
    bool sent = false;
    while (flow->retries-- > 0) {
        sent = send_tcp_segment(src_ip, dst->ip, &syn_hdr, NULL, 0);
        if (!sent) {
            break;
        }
        uint64_t wait_ms = TCP_RETRY_TIMEOUT_MS;
        uint64_t elapsed = 0;
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
    free_flow_entry(idx);
    return false;
}

tcp_result_t tcp_flow_send(tcp_data *flow_ctx) {
    if (!flow_ctx) {
        return TCP_INVALID;
    }
    tcp_flow_t *flow = NULL;
    for (int i = 0; i < MAX_TCP_FLOWS; ++i) {
        if (&tcp_flows[i].ctx == flow_ctx) {
            flow = &tcp_flows[i];
            break;
        }
    }
    if (!flow) {
        return TCP_INVALID;
    }
    
    uint8_t flags = flow_ctx->flags;
    uint8_t *payload_ptr = (uint8_t*) flow_ctx->payload.ptr;
    uint16_t payload_len = flow_ctx->payload.size;
    if (flow->state != TCP_ESTABLISHED && !(flags & (1<<FIN_F))) {
        if (!(flow->state == TCP_CLOSE_WAIT && (flags & (1<<FIN_F)))) {
            return TCP_INVALID;
        }
    }

    tcp_hdr_t hdr;
    hdr.src_port = htons(flow->local_port);
    hdr.dst_port = htons(flow->remote.port);
    hdr.sequence = htonl(flow_ctx->sequence);
    hdr.ack = htonl(flow_ctx->ack);
    hdr.flags = flags;
    hdr.window = flow_ctx->window ? flow_ctx->window : 0xFFFF;
    hdr.urgent_ptr = 0;

    uint32_t src_ip = ipv4_get_cfg()->ip;
    uint32_t dst_ip = flow->remote.ip;

    bool sent = send_tcp_segment(src_ip, dst_ip, &hdr, payload_ptr, payload_len);
    if (!sent) {
        return TCP_RESET;
    }

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
                if (flow_ctx->ack_received >= flow_ctx->expected_ack) {
                    break;
                }
                if (flow->state == TCP_STATE_CLOSED) {
                    return TCP_RESET;
                }
                sleep(interval);
                elapsed += interval;
            }
            if (flow_ctx->ack_received >= flow_ctx->expected_ack) {
                break;
            }

            if (flow->state >= TCP_CLOSING || flow->state == TCP_STATE_CLOSED) {
                break;
            }

            flow_ctx->sequence -= seq_incr;
            send_tcp_segment(src_ip, dst_ip, &hdr, payload_ptr, payload_len);
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

        flow_ctx->payload.ptr  = 0;
        flow_ctx->payload.size = 0;
        flow_ctx->flags = (1u << FIN_F) | (1u << ACK_F);

        tcp_result_t res = tcp_flow_send(flow_ctx);
        if (res != TCP_OK)  return res;

        if (flow->state == TCP_ESTABLISHED) {
            flow->state = TCP_FIN_WAIT_1;
        } else { flow->state = TCP_LAST_ACK; }

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


void tcp_input(uintptr_t ptr, uint32_t len, uint32_t src_ip, uint32_t dst_ip) {
    if (len < sizeof(tcp_hdr_t)) {
        return;
    }
    tcp_hdr_t *hdr = (tcp_hdr_t*) ptr;

    uint16_t recv_checksum = hdr->checksum;
    hdr->checksum = 0;
    uint16_t calc_checksum = tcp_compute_checksum((uint8_t*)hdr, (uint16_t)len, src_ip, dst_ip);
    hdr->checksum = recv_checksum;
    if (recv_checksum != calc_checksum) return;

    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint32_t seq = ntohl(hdr->sequence);
    uint32_t ack = ntohl(hdr->ack);
    uint8_t flags = hdr->flags;
    uint16_t window = ntohs(hdr->window);
    int idx = find_flow(dst_port, src_ip, src_port);
    tcp_flow_t *flow = (idx >= 0 ? &tcp_flows[idx] : NULL);

    if (!flow) {
        int listen_idx = find_flow(dst_port, 0, 0);
        if ((flags & (1<<SYN_F)) && !(flags & (1<<ACK_F)) && listen_idx >= 0) {
            //TODO: use a syscall for the rng
            rng_t rng;
            rng_init_random(&rng);
            tcp_flow_t *lf = &tcp_flows[listen_idx];
            int new_idx = allocate_flow_entry();
            if (new_idx < 0) return;

            flow = &tcp_flows[new_idx];
            flow->local_port = dst_port;
            flow->remote.ip = src_ip;
            flow->remote.port = src_port;
            flow->state = TCP_SYN_RECEIVED;
            flow->retries = TCP_SYN_RETRIES;
            
            uint32_t iss = rng_next32(&rng);
            flow->ctx.sequence = iss;
            flow->ctx.ack = seq + 1;
            flow->ctx.window = 0xFFFF;
            flow->ctx.flags = 0;
            flow->ctx.options.ptr = 0;
            flow->ctx.options.size = 0;
            flow->ctx.payload.ptr = 0;
            flow->ctx.payload.size = 0;
            flow->ctx.expected_ack = iss + 1;
            flow->ctx.ack_received = 0;

            tcp_hdr_t synack_hdr;
            synack_hdr.src_port = htons(dst_port);
            synack_hdr.dst_port = htons(src_port);
            synack_hdr.sequence = htonl(iss);
            synack_hdr.ack      = htonl(seq + 1);
            synack_hdr.flags    = (1<<SYN_F) | (1<<ACK_F);
            synack_hdr.window   = flow->ctx.window;
            synack_hdr.urgent_ptr = 0;
            uint32_t src_ip_local = ipv4_get_cfg()->ip;
            send_tcp_segment(src_ip_local, src_ip, &synack_hdr, NULL, 0);

            return;
        } else {
            if (!(flags & (1<<RST_F))) {
                if (flags & (1<<ACK_F)) {
                    send_reset(dst_ip, src_ip, dst_port, src_port, seq, ack, false);
                } else {
                    send_reset(dst_ip, src_ip, dst_port, src_port, seq, ack, true);
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
                final_ack.src_port = htons(flow->local_port);
                final_ack.dst_port = htons(flow->remote.port);
                final_ack.sequence = htonl(flow->ctx.sequence + 1);
                final_ack.ack      = htonl(flow->ctx.ack);
                final_ack.flags    = (1<<ACK_F);
                final_ack.window   = flow->ctx.window;
                final_ack.urgent_ptr = 0;
                uint32_t src_ip_local = ipv4_get_cfg()->ip;
                send_tcp_segment(src_ip_local, flow->remote.ip, &final_ack, NULL, 0);

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
                port_recv_handler_t h = port_get_handler(PROTO_TCP, dst_port);
                if (h) {
                    h(0, 0, src_ip, src_port, dst_port);
                }
            }
        } else if (flags & (1<<RST_F)) {
            free_flow_entry(idx);
        }
        return;
    case TCP_ESTABLISHED: //keep alive
        uint8_t hdr_len  = (hdr->data_offset_reserved >> 4) * 4;
        uint32_t data_len = len - hdr_len;

        if ((flags & ACK_F)
            && !(flags & (SYN_F|FIN_F|RST_F))
            && data_len == 0
            && seq + 1 == flow->ctx.ack)
        {
            tcp_hdr_t ack_hdr = {
                .src_port    = htons(flow->local_port),
                .dst_port    = htons(flow->remote.port),
                .sequence    = htonl(flow->ctx.sequence),
                .ack         = htonl(flow->ctx.ack),
                .flags       = (1 << ACK_F),
                .window      = flow->ctx.window,
                .urgent_ptr  = 0
            };
            send_tcp_segment(
                ipv4_get_cfg()->ip,
                flow->remote.ip,
                &ack_hdr, NULL, 0
            );
            return;
        }
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
    case TCP_CLOSE_WAIT:
    case TCP_CLOSING:
    case TCP_LAST_ACK: {
    if (flags & (1 << RST_F)) {
        free_flow_entry(idx);
        return;
    }
    uint8_t hdr_len =(hdr->data_offset_reserved >> 4) * 4;
    if (len < hdr_len)
        return;
    uint32_t data_len = len - hdr_len;
    bool fin_set = (flags & (1 << FIN_F)) != 0;
    bool fin_inseq = fin_set && (seq == flow->ctx.ack);

    if (data_len && seq == flow->ctx.ack) {
        flow->ctx.ack += data_len;
        port_recv_handler_t h = port_get_handler(PROTO_TCP, dst_port);
        if (h) h(ptr + hdr_len, data_len, src_ip, src_port, dst_port);
    }

    if (fin_inseq) {
        flow->ctx.ack += 1;
    }

    if ((data_len && seq == flow->ctx.ack - data_len) || fin_inseq) {
        tcp_hdr_t ackhdr = {
            .src_port = htons(flow->local_port),
            .dst_port = htons(flow->remote.port),
            .sequence = htonl(flow->ctx.sequence),
            .ack = htonl(flow->ctx.ack),
            .flags = (1 << ACK_F),
            .window = flow->ctx.window,
            .urgent_ptr = 0
        };
        send_tcp_segment(ipv4_get_cfg()->ip,
                         flow->remote.ip, &ackhdr, NULL, 0);
    }

    if ((flags & (1 << ACK_F)) &&
        ack > flow->ctx.ack_received) {
        flow->ctx.ack_received = ack;

        if (flow->state == TCP_FIN_WAIT_1 &&
            ack == flow->ctx.expected_ack) {
            flow->state = TCP_FIN_WAIT_2;
        } else if ((flow->state == TCP_LAST_ACK ||
                    flow->state == TCP_CLOSING) &&
                   ack == flow->ctx.expected_ack) {
            free_flow_entry(idx);
            return;
        }
    }

    if (fin_inseq) {
        if (flow->state == TCP_ESTABLISHED)
            flow->state = TCP_CLOSE_WAIT;
        else if (flow->state == TCP_FIN_WAIT_1)
            flow->state = TCP_CLOSING;
        else if (flow->state == TCP_FIN_WAIT_2)
            flow->state = TCP_TIME_WAIT;
        else if (flow->state == TCP_CLOSING)
            flow->state = TCP_TIME_WAIT;
        else if (flow->state == TCP_LAST_ACK)
            flow->state = TCP_TIME_WAIT;
    }
    return;
}
    default:
        break;
    }
}
