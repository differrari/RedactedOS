#pragma once
#include "types.h"
#include "net/network_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __attribute__((packed)) arp_hdr_t {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t opcode;//1=request, 2=reply
    uint8_t sender_mac[6];
    uint32_t sender_ip;
    uint8_t target_mac[6];
    uint32_t target_ip;
} arp_hdr_t;

bool arp_should_handle(const arp_hdr_t *arp, uint32_t my_ip);
void arp_populate_response(net_l2l3_endpoint *ep, const arp_hdr_t *arp);
bool arp_resolve(uint32_t ip, uint8_t mac_out[6], uint32_t timeout_ms);

#define ARP_TABLE_MAX  64

typedef struct arp_entry {
    uint32_t ip;
    uint8_t  mac[6];
    uint32_t ttl_ms;
    uint8_t  static_entry;//1 static, 0 dynamic
} arp_entry_t;

void arp_table_init();

void arp_table_put(uint32_t ip, const uint8_t mac[6], uint32_t ttl_ms, bool is_static);

bool arp_table_get(uint32_t ip, uint8_t mac_out[6]);

void arp_table_tick(uint32_t ms);

void arp_table_init_static_defaults();

void arp_send_request(uint32_t target_ip);

void arp_daemon_entry();
bool arp_can_reply();
void arp_daemon_entry();
void arp_set_pid(uint16_t pid);
uint16_t arp_get_pid();
void arp_input(uintptr_t frame_ptr, uint32_t frame_len);
#ifdef __cplusplus
}
#endif
