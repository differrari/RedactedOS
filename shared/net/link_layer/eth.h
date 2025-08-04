#pragma once
#include "types.h"
#include "net/network_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __attribute__((packed)) eth_hdr_t {
    uint8_t dst_mac[6];
    uint8_t src_mac[6];
    uint16_t ethertype;
} eth_hdr_t;

uintptr_t create_eth_packet(uintptr_t ptr,
                            const uint8_t src_mac[6],
                            const uint8_t dst_mac[6],
                            uint16_t type);

uint16_t eth_parse_packet_type(uintptr_t ptr);

const uint8_t* eth_get_source(uintptr_t ptr);

bool eth_send_frame(uintptr_t frame_ptr, uint32_t frame_len);
void eth_input(uintptr_t frame_ptr, uint32_t frame_len);

#ifdef __cplusplus
}
#endif
