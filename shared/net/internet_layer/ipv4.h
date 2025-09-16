#pragma once
#include "types.h"
#include "std/string.h"
#include "net/link_layer/eth.h"
#include "net/network_types.h"
#include "net/checksums.h"
#include "networking/interface_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __attribute__((packed)) ipv4_hdr_t {
    uint8_t version_ihl;
    uint8_t dscp_ecn;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_frag_offset;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t header_checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} ipv4_hdr_t;

typedef struct {
    uint8_t index;
    bool int_type; //0 = l2 index, 1= l3 index 
} ipv4_tx_opts_t;

void ipv4_to_string(uint32_t ip, char* buf);

void ipv4_send_packet(uint32_t dst_ip,
                      uint8_t proto,
                      sizedptr segment,
                      const ipv4_tx_opts_t* opts);
                      
void ipv4_input(uint16_t ifindex, uintptr_t ip_ptr,
              uint32_t ip_len,
              const uint8_t src_mac[6]);






//LEGACY

typedef struct net_cfg {
    uint32_t ip;
    uint32_t mask;
    uint32_t gw;
    int8_t mode;
    net_runtime_opts_t *rt;
} net_cfg_t;
void ipv4_cfg_init(void);
void ipv4_set_cfg(const net_cfg_t *src);
const net_cfg_t* ipv4_get_cfg(void); 

#ifdef __cplusplus
}
#endif
