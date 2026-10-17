#pragma once
#include "mdns_responder.h"
#include "dns_wire.h"
#include "networking/interface_manager.h"

#define MDNS_TTL_S 120
#define MDNS_MAX_SERVICES 8
#define MDNS_FLUSH_CLASS (DNS_CLASS_CACHE_FLUSH | DNS_CLASS_IN)
#define MDNS_GOODBYE_BURST 1
#define MDNS_PROBE_RETRY_MS 5000
#define MDNS_HOST_NAME "RedactedOS"

typedef struct {
    bool ready;
    uint8_t sent;
    uint64_t next_ms;
} mdns_probe_t;

typedef struct {
    mdns_probe_t probe;
    uint8_t announce_left;
    uint64_t last_tx_ms;
} mdns_link_state_t;

typedef struct {
    bool used;
    bool active;
    uint8_t goodbye_left;
    uint64_t last_tx_ms;
    char instance[64];
    char service[32];
    char proto[8];
    char txt[128];
    uint16_t port;
    bool advertised;
    bool retire;
    uint16_t name_index;
    uint16_t old_name_index;
    mdns_link_state_t link[MAX_L2_INTERFACES];
} mdns_service_t;

typedef struct {
    uint8_t* out;
    uint32_t cap;
    uint32_t off;
    uint16_t an;
    uint16_t ar;
} mdns_pkt_t;

typedef struct {
    socket_handle_t sock;
    ip_version_t ver;
    uint8_t ifindex;
    l3_id_t l3_id;
    uint32_t l3_generation;
    uint8_t mcast_ip[16];
} mdns_tx_target_t;

extern char g_mdns_fqdn[72];
extern uint16_t g_mdns_host_name_index;
extern uint16_t g_mdns_old_host_name_index;
extern bool g_mdns_host_advertised;
extern uint8_t g_mdns_host_goodbye_left;
extern mdns_link_state_t g_mdns_host_link[MAX_L2_INTERFACES];
extern mdns_service_t g_mdns_services[MDNS_MAX_SERVICES];

bool mdns_has_work(void);
void mdns_reprobe(uint32_t l2_mask);
void mdns_responder_tick_multi(const mdns_tx_target_t *targets, uint32_t target_count);
void mdns_rx(socket_handle_t sock, l3_id_t l3_id, ip_version_t ver, const uint8_t *mcast_ip, const uint8_t *pkt, uint32_t pkt_len, const net_l4_endpoint *src);

bool mdns_send(socket_handle_t sock, const net_l4_endpoint *src, bool unicast, ip_version_t ver, const uint8_t *mcast_ip, const uint8_t *pkt, uint32_t pkt_len);
l2_interface_t* mdns_l2(l3_id_t l3_id);
void mdns_instance_name(char* out, uint32_t out_cap, const mdns_service_t* s, uint16_t name_index);
void mdns_record(dns_record_t* r, const char* name, uint16_t type);
uint32_t mdns_host_records(l2_interface_t* l2, dns_record_t* out, uint32_t cap);
void mdns_service_records(const mdns_service_t* s, uint16_t name_index, dns_record_t out[2]);
void mdns_cache_host(l2_interface_t* l2);
void mdns_probe_start(mdns_probe_t* probe, uint32_t delay_ms);
bool mdns_pkt_begin(mdns_pkt_t *p, uint8_t *out, uint32_t cap, uint16_t flags);
bool mdns_pkt_add(mdns_pkt_t* p, bool additional, const dns_record_t* record, uint16_t rrclass, uint32_t ttl_s);
bool mdns_send_mcast(socket_handle_t sock, ip_version_t ver, const uint8_t* mcast_ip, uint32_t l2_slot, const uint8_t* pkt, uint32_t pkt_len, uint64_t now);
