#pragma once

#include "networking/port_manager.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/link_layer/eth.h"
#include "std/memory.h"
#include "net/network_types.h"
#include "net/socket_types.h"

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

#define MAX_TCP_FLOWS 512
#define TCP_SYN_RETRIES 5
#define TCP_DATA_RETRIES 5
#define TCP_RETRY_TIMEOUT_MS 200
#define TCP_RECV_WINDOW 65535
#define TCP_MAX_TX_SEGS 16
#define TCP_INIT_RTO 200
#define TCP_MIN_RTO 200
#define TCP_MAX_RTO 60000
#define TCP_MSL_MS 30000
#define TCP_2MSL_MS (2 * TCP_MSL_MS)
#define TCP_MAX_RETRANS 8
#define TCP_MAX_PERSIST_PROBES 8

int find_flow(uint16_t local_port, ip_version_t ver, const void *local_ip, const void *remote_ip, uint16_t remote_port);
tcp_data* tcp_get_ctx(uint16_t local_port, ip_version_t ver, const void *local_ip, const void *remote_ip, uint16_t remote_port);

bool tcp_bind_l3(uint8_t l3_id, uint16_t port, uint16_t pid, port_recv_handler_t handler, const SocketExtraOptions* extra);
int tcp_alloc_ephemeral_l3(uint8_t l3_id, uint16_t pid, port_recv_handler_t handler);
bool tcp_unbind_l3(uint8_t l3_id, uint16_t port, uint16_t pid);
bool tcp_handshake_l3(uint8_t l3_id, uint16_t local_port, net_l4_endpoint *dst, tcp_data *flow_ctx, uint16_t pid, const SocketExtraOptions* extra);

tcp_result_t tcp_flow_send(tcp_data *flow_ctx);
tcp_result_t tcp_flow_close(tcp_data *flow_ctx);

void tcp_flow_window_update(tcp_data *flow_ctx);
void tcp_flow_on_app_read(tcp_data *flow_ctx, uint32_t bytes_read);

void tcp_input(ip_version_t ipver, const void *src_ip_addr, const void *dst_ip_addr, uint8_t l3_id, uintptr_t ptr, uint32_t len);

void tcp_tick_all(uint32_t elapsed_ms);
int tcp_daemon_entry(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif
