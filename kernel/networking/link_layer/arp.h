#pragma once
#include "types.h"
#include "networking/netpkt.h"
#include "networking/link_layer/link_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARP_TABLE_MAX 64
#define ARP_PENDING_MAX 8
#define ARP_PENDING_MAX_BYTES 32768
#define ARP_OPCODE_REQUEST 1
#define ARP_OPCODE_REPLY 2

typedef enum {
    ARP_STATE_UNUSED = 0,
    ARP_STATE_INCOMPLETE = 1,
    ARP_STATE_REACHABLE = 2,
    ARP_STATE_STALE = 3
} arp_state_t;

typedef struct __attribute__((packed)) arp_hdr_t {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t opcode;//1=request, 2=reply
    uint8_t sender_mac[MAC_ADDR_LEN];
    uint32_t sender_ip;
    uint8_t target_mac[MAC_ADDR_LEN];
    uint32_t target_ip;
} arp_hdr_t;

typedef struct arp_entry {
    uint32_t ip;
    uint8_t mac[MAC_ADDR_LEN];
    uint32_t ttl_ms;
    uint32_t timer_ms;
    uint32_t pending_bytes;
    netpkt_t** pending;
    uint8_t pending_len;
    uint8_t state;
    uint8_t probes_sent;
    uint8_t static_entry;//1 static, 0 dynamic
} arp_entry_t;

typedef struct arp_table arp_table_t;

arp_table_t* arp_table_create(void);
void arp_table_destroy(arp_table_t* t);
void arp_table_init_static_defaults(arp_table_t* t);

void arp_table_put_for_l2(uint8_t ifindex, uint32_t ip, const uint8_t mac[MAC_ADDR_LEN], uint32_t ttl_ms, bool is_static);
bool arp_table_get_for_l2(uint8_t ifindex, uint32_t ip, uint8_t mac_out[MAC_ADDR_LEN]);
bool arp_table_delete_for_l2(uint8_t ifindex, uint32_t ip);
uint32_t arp_table_dump_for_l2(uint8_t ifindex, arp_entry_t* out, uint32_t out_cap);
bool arp_send_or_queue_on(uint8_t ifindex, uint32_t ip, netpkt_t* pkt);
bool arp_send_request_on(uint8_t ifindex, uint32_t sender_ip, uint32_t target_ip);
bool arp_dad_ipv4_on(uint8_t ifindex, uint32_t ip);

void arp_input(uint8_t ifindex, const uint8_t src_mac[MAC_ADDR_LEN], netpkt_t* pkt);

int arp_daemon_entry(int argc, char* argv[]);

#ifdef __cplusplus
}
#endif