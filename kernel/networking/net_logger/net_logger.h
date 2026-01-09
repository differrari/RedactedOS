#pragma once
#include "types.h"
#include "net/network_types.h"
#include "net/socket_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NETLOG_COMP_UDP = 0,
    NETLOG_COMP_TCP = 1,
    NETLOG_COMP_HTTP_CLIENT = 2,
    NETLOG_COMP_HTTP_SERVER = 3
} netlog_component_t;

typedef enum {
    NETLOG_ACT_BIND = 0,
    NETLOG_ACT_CONNECT = 1,
    NETLOG_ACT_CONNECTED = 2,
    NETLOG_ACT_LISTEN = 3,
    NETLOG_ACT_ACCEPT = 4,
    NETLOG_ACT_SEND = 5,
    NETLOG_ACT_SENDTO = 6,
    NETLOG_ACT_RECV = 7,
    NETLOG_ACT_RECVFROM = 8,
    NETLOG_ACT_CLOSE = 9,
    NETLOG_ACT_HTTP_SEND_REQUEST = 10,
    NETLOG_ACT_HTTP_RECV_RESPONSE = 11,
    NETLOG_ACT_HTTP_RECV_REQUEST = 12,
    NETLOG_ACT_HTTP_SEND_RESPONSE = 13
} netlog_action_t;

typedef struct netlog_socket_event_t {
    netlog_component_t comp;
    netlog_action_t action;

    int64_t i0;
    int64_t i1;
    uint32_t u0;
    uint32_t u1;

    uint16_t pid;
    uint16_t local_port;

    SockBindSpec bind_spec;

    SockDstKind dst_kind;
    net_l4_endpoint dst_ep;
    net_l4_endpoint remote_ep;

    const char* s0;
    const char* s1;
} netlog_socket_event_t;

void netlog_socket_event(const SocketExtraOptions*extra, const netlog_socket_event_t* e);

#ifdef __cplusplus
}
#endif