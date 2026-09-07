#pragma once

#include "types.h"
#include "networking/interface_manager.h"
#include "networking/link_layer/link_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DHCPV6_MSG_SOLICIT = 1,
    DHCPV6_MSG_ADVERTISE = 2,
    DHCPV6_MSG_REQUEST = 3,
    DHCPV6_MSG_CONFIRM = 4,
    DHCPV6_MSG_RENEW = 5,
    DHCPV6_MSG_REBIND = 6,
    DHCPV6_MSG_REPLY = 7,
    DHCPV6_MSG_RELEASE = 8,
    DHCPV6_MSG_DECLINE = 9,
    DHCPV6_MSG_INFORMATION_REQUEST = 11
};

#define DHCPV6_CLIENT_PORT 546
#define DHCPV6_SERVER_PORT 547

#define DHCPV6_OPT_CLIENTID 1
#define DHCPV6_OPT_SERVERID 2
#define DHCPV6_OPT_IA_NA 3
#define DHCPV6_OPT_IAADDR 5
#define DHCPV6_OPT_ORO 6
#define DHCPV6_OPT_ELAPSED 8
#define DHCPV6_OPT_STATUS_CODE 13
#define DHCPV6_OPT_DNS_SERVERS 23
#define DHCPV6_OPT_NTP_SERVER 56
#define DHCPV6_OPT_IA_PD 25
#define DHCPV6_OPT_IAPREFIX 26
#define DHCPV6_OPT_INFORMATION_REFRESH_TIME 32
#define DHCPV6_OPT_SOL_MAX_RT 82
#define DHCPV6_OPT_INF_MAX_RT 83

#define DHCPV6_NTP_SUBOPT_SERVER_ADDR 1

#define DHCPV6_MAX_SERVER_ID 128
#define DHCPV6_MAX_CLIENT_ID 128
#define DHCPV6_MAX_MSG 512

#define DHCPV6_STATUS_SUCCESS 0
#define DHCPV6_STATUS_NO_BINDING 3
#define DHCPV6_STATUS_NOT_ON_LINK 4

typedef struct {
    uint8_t msg_type;
    uint32_t xid24;

    bool has_server_id;
    uint16_t server_id_len;
    uint8_t server_id[DHCPV6_MAX_SERVER_ID];

    bool has_client_id;
    uint16_t client_id_len;
    uint8_t client_id[DHCPV6_MAX_CLIENT_ID];

    bool has_status;
    uint16_t status_code;
    bool has_ia_status;
    uint16_t ia_status_code;

    bool has_ia_na;
    bool has_addr;
    uint8_t addr[16];
    bool has_expired_addr;
    uint8_t expired_addr[16];
    uint32_t preferred_lft;
    uint32_t valid_lft;

    uint32_t t1;
    uint32_t t2;

    bool has_dns;
    uint8_t dns[2][16];

    bool has_ntp;
    uint8_t ntp[2][16];

    bool has_pd;
    uint8_t pd_prefix[16];
    uint8_t pd_prefix_len;
    uint32_t pd_preferred_lft;
    uint32_t pd_valid_lft;

    bool has_sol_max_rt;
    uint32_t sol_max_rt;
    bool has_inf_max_rt;
    uint32_t inf_max_rt;
    bool has_info_refresh_time;
    uint32_t info_refresh_time;
} dhcpv6_parsed_t;

uint32_t dhcpv6_make_xid24(uint32_t r32);

uint32_t dhcpv6_iaid_from_mac(const uint8_t mac[MAC_ADDR_LEN]);

bool dhcpv6_build_message(uint8_t* out, uint32_t out_cap, uint32_t* out_len, const net_runtime_opts_v6_t* rt, const uint8_t mac[MAC_ADDR_LEN], uint8_t type, uint32_t xid24, uint16_t elapsed_cs, const uint8_t ia_addr[16]);

bool dhcpv6_parse_message(const uint8_t *msg, uint32_t msg_len, uint32_t expect_xid24, uint32_t expect_iaid, dhcpv6_parsed_t *out);

#ifdef __cplusplus
}
#endif