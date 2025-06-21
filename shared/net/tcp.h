#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "net/network_types.h"

#define FIN_F 0
#define SYN_F 1
#define RST_F 2
#define PSH_F 3
#define ACK_F 4
#define URG_F 5
#define ECE_F 6
#define CWR_F 7

#define TCP_RESET 2
#define TCP_RETRY 1
#define TCP_OK 0

typedef struct __attribute__((packed)) tcp_hdr_t {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t sequence;
    uint32_t ack;
    uint8_t data_offset_reserved;// upper offset, lower reserved
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} tcp_hdr_t;

typedef struct tcp_data {
    uint32_t sequence;
    uint32_t ack;
    uint8_t padding;
    uint8_t flags;
    uint16_t window;
    sizedptr options;
    sizedptr payload;
    uint32_t expected_ack;
} tcp_data;

void create_tcp_packet(uintptr_t p, network_connection_ctx source, network_connection_ctx destination, sizedptr payload);
size_t calc_tcp_size(uint16_t payload_len);
uint16_t tcp_parse_packet(uintptr_t ptr);
sizedptr tcp_parse_packet_payload(uintptr_t ptr);

void tcp_send(uint16_t port, network_connection_ctx *destination, tcp_data* data);
void tcp_reset(uint16_t port, network_connection_ctx *destination, tcp_data* data);
bool expect_response(sizedptr *pack);
uint8_t tcp_check_response(tcp_data *data);
bool tcp_handskake(network_connection_ctx *dest, uint16_t port, tcp_data *data, uint8_t retry);
bool tcp_close(network_connection_ctx *dest, uint16_t port, tcp_data *data, uint8_t retry, uint32_t orig_seq, uint32_t orig_ack);

#ifdef __cplusplus
}
#endif