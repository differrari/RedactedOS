#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ndp_table ndp_table_t;

#define RA_FLAG_M 0x80
#define RA_FLAG_O 0x40

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
    uint8_t state;
    uint8_t probes_sent;
    uint8_t is_router;
    uint32_t router_lifetime_ms;
} ndp_entry_t;

#define NDP_TABLE_MAX 64

ndp_table_t* ndp_table_create(void);
void ndp_table_destroy(ndp_table_t* t);

void ndp_input(uint16_t ifindex, const uint8_t src_ip[16], const uint8_t dst_ip[16], const uint8_t src_mac[6], const uint8_t* icmp, uint32_t icmp_len);

void ndp_table_put_for_l2(uint8_t ifindex, const uint8_t ip[16], const uint8_t mac[6], uint32_t ttl_ms, bool router);

bool ndp_resolve_on(uint16_t ifindex, const uint8_t next_hop[16], uint8_t out_mac[6], uint32_t timeout_ms);

bool ndp_request_dad_on(uint8_t ifindex, const uint8_t ip[16]);

int ndp_daemon_entry(int argc, char* argv[]);

#ifdef __cplusplus
}
#endif
