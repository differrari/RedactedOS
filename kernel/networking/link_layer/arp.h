#pragma once
#include "types.h"
#include "networking/interface_manager.h"
#include "networking/netpkt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARP_TABLE_MAX 64
#define ARP_OPCODE_REQUEST 1
#define ARP_OPCODE_REPLY 2

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

typedef struct arp_entry {
    uint32_t ip;
    uint8_t  mac[6];
    uint32_t ttl_ms;
    uint8_t  static_entry;//1 static, 0 dynamic
} arp_entry_t;

typedef struct arp_table arp_table_t;

arp_table_t* arp_table_create(void);
void arp_table_destroy(arp_table_t* t);
void arp_table_init_static_defaults(arp_table_t* t);

void arp_table_put_for_l2(uint8_t ifindex, uint32_t ip, const uint8_t mac[6], uint32_t ttl_ms, bool is_static);
bool arp_table_get_for_l2(uint8_t ifindex, uint32_t ip, uint8_t mac_out[6]);
void arp_table_tick_for_l2(uint8_t ifindex, uint32_t ms);
void arp_tick_all(uint32_t ms);

bool arp_resolve_on(uint8_t ifindex, uint32_t ip, uint8_t mac_out[6], uint32_t timeout_ms);
void arp_send_request_on(uint8_t ifindex, uint32_t target_ip);

void arp_input(uint16_t ifindex, netpkt_t* pkt);

void arp_set_pid(uint16_t pid);
uint16_t arp_get_pid(void);
int arp_daemon_entry(int argc, char* argv[]);

#ifdef __cplusplus
}
#endif