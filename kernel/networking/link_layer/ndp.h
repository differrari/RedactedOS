#pragma once

#include "types.h"
#include "networking/netpkt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ndp_table ndp_table_t;

#define RA_FLAG_M 0x80
#define RA_FLAG_O 0x40
#define NDP_PENDING_MAX 8
#define NDP_PENDING_MAX_BYTES 32768

typedef enum {
    NDP_STATE_UNUSED = 0,
    NDP_STATE_INCOMPLETE = 1,
    NDP_STATE_REACHABLE = 2,
    NDP_STATE_STALE = 3,
    NDP_STATE_DELAY = 4,
    NDP_STATE_PROBE = 5
} ndp_state_t;

typedef struct {
    uint8_t ip[16];
    uint8_t mac[6];
    uint32_t ttl_ms;
    uint32_t timer_ms;
    uint32_t pending_bytes;
    netpkt_t** pending;
    uint8_t pending_len;
    uint8_t state;
    uint8_t probes_sent;
    uint8_t is_router;
    uint8_t static_entry;
    uint32_t router_lifetime_ms;
} ndp_entry_t;

#define NDP_TABLE_MAX 64

ndp_table_t* ndp_table_create(void);
void ndp_table_destroy(ndp_table_t* t);

void ndp_input(uint16_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], const uint8_t src_mac[6], netpkt_t* pkt);

void ndp_table_put_for_l2(uint8_t ifindex, const uint8_t ip[16], const uint8_t mac[6], uint32_t ttl_ms, bool router, bool is_static);
bool ndp_table_get_for_l2(uint8_t ifindex, const uint8_t ip[16], uint8_t mac_out[6]);
bool ndp_table_delete_for_l2(uint8_t ifindex, const uint8_t ip[16]);
uint32_t ndp_table_dump_for_l2(uint8_t ifindex, ndp_entry_t* out, uint32_t out_cap);

bool ndp_send_or_queue_on(uint16_t ifindex, const uint8_t next_hop[16], netpkt_t* pkt);

bool ndp_request_dad_on(uint8_t ifindex, const uint8_t ip[16]);

int ndp_daemon_entry(int argc, char* argv[]);

#ifdef __cplusplus
}
#endif
