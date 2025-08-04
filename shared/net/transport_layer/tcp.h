#pragma once

#include "networking/port_manager.h"
#include "net/internet_layer/ipv4.h"
#include "net/link_layer/eth.h"
#include "std/memfunctions.h"
#include "net/network_types.h"
#ifdef __cplusplus
extern "C" {
#endif

#define FIN_F 0
#define SYN_F 1
#define RST_F 2
#define PSH_F 3
#define ACK_F 4
#define URG_F 5
#define ECE_F 6
#define CWR_F 7

typedef enum {
    TCP_OK          = 0,
    TCP_RETRY       = 1,
    TCP_RESET       = 2,
    TCP_TIMEOUT     = -2,
    TCP_CSUM_ERR    = -3,
    TCP_INVALID     = -4,
    TCP_WOULDBLOCK  = -5,
    TCP_DISCONNECT  = -6,
    TCP_UNIMPLEMENT = -10,
    TCP_BUSY        = -11,
} tcp_result_t;

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t sequence;
    uint32_t ack;
    uint8_t data_offset_reserved;
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} tcp_hdr_t;

typedef struct {
    uint32_t sequence;
    uint32_t ack;
    uint8_t flags;
    uint16_t window;
    sizedptr options;
    sizedptr payload;
    uint32_t expected_ack;
    uint32_t ack_received;
} tcp_data;

typedef enum {
    TCP_STATE_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
} tcp_state_t;

typedef struct {
    uint16_t local_port;
    net_l4_endpoint remote;
    tcp_state_t state; 
    tcp_data ctx;
    uint8_t retries;
} tcp_flow_t;

#define MAX_TCP_FLOWS 512
#define TCP_SYN_RETRIES 5
#define TCP_DATA_RETRIES 5
#define TCP_RETRY_TIMEOUT_MS 1000

static int find_flow(uint16_t local_port, uint32_t remote_ip, uint16_t remote_port);

tcp_data* tcp_get_ctx(uint16_t local_port,
                    uint32_t remote_ip,
                    uint16_t remote_port);

bool tcp_bind(uint16_t port,
                uint16_t pid,
                port_recv_handler_t handler);

int tcp_alloc_ephemeral(uint16_t pid,
                        port_recv_handler_t handler);

bool tcp_unbind(uint16_t port,
                uint16_t pid);

bool tcp_handshake(uint16_t local_port,
                    net_l4_endpoint *dst,
                    tcp_data *flow_ctx,
                    uint16_t pid);

tcp_result_t tcp_flow_send(tcp_data *flow_ctx);

tcp_result_t tcp_flow_close(tcp_data *flow_ctx);

void tcp_input(uintptr_t ptr,
                    uint32_t len,
                    uint32_t src_ip,
                    uint32_t dst_ip);

#ifdef __cplusplus
}
#endif
