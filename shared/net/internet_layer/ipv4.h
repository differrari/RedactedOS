#pragma once
#include "types.h"
#include "std/string.h"
#include "net/link_layer/eth.h"
#include "net/network_types.h"
#include "net/checksums.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NET_MODE_DISABLED ((int8_t)-1)
#define NET_MODE_DHCP 0
#define NET_MODE_STATIC 1

typedef struct net_runtime_opts {
    uint16_t mtu;
    uint32_t t1;
    uint32_t t2;
    uint32_t dns[2];
    uint32_t ntp[2];
    uint16_t xid;
    uint32_t server_ip;
    uint32_t lease;
} net_runtime_opts_t;

typedef struct net_cfg {
    uint32_t ip;
    uint32_t mask;
    uint32_t gw;
    int8_t mode;
    net_runtime_opts_t *rt;
} net_cfg_t;

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

void ipv4_cfg_init();
void ipv4_set_cfg(const net_cfg_t *src);
const net_cfg_t* ipv4_get_cfg();

string ipv4_to_string(uint32_t ip);

void ipv4_send_segment(uint32_t src_ip,
                            uint32_t dst_ip,
                            uint8_t  proto,
                            sizedptr segment);

void ip_input(uintptr_t ip_ptr,
            uint32_t ip_len,
            const uint8_t src_mac[6]);

static inline uint32_t ipv4_network(uint32_t ip, uint32_t mask){ return ip & mask; }
static inline uint32_t ipv4_broadcast(uint32_t ip, uint32_t mask){ return (ip & mask) | ~mask; }
static inline uint32_t ipv4_first_host(uint32_t ip, uint32_t mask){ return (ip & mask) + 1; }
static inline uint32_t ipv4_last_host(uint32_t ip, uint32_t mask){ return ((ip & mask) | ~mask) - 1; }

void ipv4_cfg_init();
void ipv4_set_cfg(const net_cfg_t *src);
const net_cfg_t* ipv4_get_cfg();
#ifdef __cplusplus
}
#endif
