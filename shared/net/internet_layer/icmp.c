#include "icmp.h"
#include "net/checksums.h"
#include "std/memory.h"
#include "networking/interface_manager.h"
#include "net/internet_layer/ipv4.h"
#include "net/internet_layer/ipv4_route.h"
#include "syscalls/syscalls.h"

#define MAX_PENDING 16
#define POLL_MS 1

typedef struct {
    bool in_use;
    uint16_t id, seq;
    bool received;
    uint8_t rx_type;
    uint8_t rx_code;
    uint32_t start_ms;
    uint32_t end_ms;
} ping_slot_t;

static ping_slot_t g_pending[MAX_PENDING] = {0};

static int  alloc_slot(uint16_t id, uint16_t seq){
    for(int i=0;i<MAX_PENDING;i++){
        if(!g_pending[i].in_use){
            g_pending[i].in_use = true;
            g_pending[i].id = id;
            g_pending[i].seq = seq;
            g_pending[i].received = false;
            g_pending[i].rx_type = 0xFF;
            g_pending[i].rx_code = 0xFF;
            g_pending[i].start_ms = (uint32_t)get_time();
            g_pending[i].end_ms = 0;
            return i;
        }
    }
    return -1;
}

static void mark_received(uint16_t id,uint16_t seq,uint8_t type,uint8_t code) {
    for(int i=0;i<MAX_PENDING;i++) {
        if (g_pending[i].in_use&&g_pending[i].id==id&&g_pending[i].seq==seq) {
            g_pending[i].received=true;
            g_pending[i].rx_type=type;
            g_pending[i].rx_code=code;
            g_pending[i].end_ms=(uint32_t)get_time();
            return;
        }
    }
}

static void free_slot(int i){
    if(i>=0&&i<MAX_PENDING) g_pending[i].in_use=false;
}

void create_icmp_packet(uintptr_t p, const icmp_data *d) {
    icmp_packet *pkt = (icmp_packet*)p;
    pkt->type = d->response ? ICMP_ECHO_REPLY : ICMP_ECHO_REQUEST;
    pkt->code = 0;
    pkt->id   = bswap16(d->id);
    pkt->seq  = bswap16(d->seq);

    memset(pkt->payload, 0, sizeof(pkt->payload));
    memcpy(pkt->payload, d->payload, sizeof(pkt->payload));

    pkt->checksum = 0;
}

void icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq, const uint8_t payload[56]) {
    uint32_t pay_len = payload ? 56 : 32;
    icmp_data d = { .response=false, .id=id, .seq=seq };
    if (payload) memcpy(d.payload, payload, 56);
    else memset(d.payload, 0, 56);

    uint32_t icmp_len = 8 + pay_len;
    uintptr_t buf = (uintptr_t)malloc(icmp_len);
    if(!buf) return;

    create_icmp_packet(buf, &d);

    ((icmp_packet*)buf)->checksum = checksum16((uint16_t*)buf, icmp_len);

    ipv4_send_packet(dst_ip, 1, (sizedptr){ buf, icmp_len}, NULL);

    free((void*)buf, icmp_len);
}

void icmp_input(uintptr_t ptr, uint32_t len, uint32_t src_ip, uint32_t dst_ip) {
    if (len < 8) return;

    icmp_packet *pkt = (icmp_packet*)ptr;
    uint16_t recv_ck = pkt->checksum;
    pkt->checksum = 0;
    if (checksum16((uint16_t*)pkt, len) != recv_ck) return;
    pkt->checksum = recv_ck;

    uint8_t type = pkt->type;
    uint8_t code = pkt->code;
    uint16_t id = bswap16(pkt->id);
    uint16_t sq = bswap16(pkt->seq);
    uint32_t pay = len - 8;
    if (pay > 56) pay = 56;

    if (type == ICMP_ECHO_REQUEST) {
        icmp_data d = { .response = true, .id = id, .seq = sq };
        memcpy(d.payload, pkt->payload, pay);
        memset(d.payload + pay, 0, 56 - pay);

        uint32_t reply_len = 8 + pay;
        uintptr_t buf = (uintptr_t)malloc(reply_len);
        if(!buf) return;

        create_icmp_packet(buf, &d);
        ((icmp_packet*)buf)->checksum = checksum16((uint16_t*)buf, reply_len);

        l3_ipv4_interface_t *l3 = l3_ipv4_find_by_ip(dst_ip);
        if (l3 && l3->l2) {
            ipv4_tx_opts_t o = { .index = l3->l3_id, .scope = IPV4_TX_BOUND_L3 };
            ipv4_send_packet(src_ip, 1, (sizedptr){ buf, reply_len }, &o);
        }

        free((void*)buf, reply_len);
        return;
    }

    if(type == ICMP_ECHO_REPLY) {
        mark_received(id, sq, type, 0);
        return;
    }

    if (type == ICMP_DEST_UNREACH || type == ICMP_TIME_EXCEEDED || type == ICMP_PARAM_PROBLEM || type == ICMP_REDIRECT) {
        mark_received(id, sq, type, code);
        return;
    }
}

static ping_status_t map_status(uint8_t t, uint8_t c) {
    if (t == ICMP_ECHO_REPLY) return PING_OK;
    if (t == ICMP_TIME_EXCEEDED) return PING_TTL_EXPIRED;
    if (t == ICMP_PARAM_PROBLEM) return PING_PARAM_PROBLEM;
    if (t == ICMP_REDIRECT) return PING_REDIRECT;
    if (t == ICMP_DEST_UNREACH) {
        switch(c) {
            case 0: return PING_NET_UNREACH;
            case 1: return PING_HOST_UNREACH;
            case 2: return PING_PROTO_UNREACH;
            case 3: return PING_PORT_UNREACH;
            case 4: return PING_FRAG_NEEDED;
            case 5: return PING_SRC_ROUTE_FAILED;
            case 13: return PING_ADMIN_PROHIBITED;
            default: return PING_UNKNOWN_ERROR;
        }
    }
    return PING_UNKNOWN_ERROR;
}

bool icmp_ping(uint32_t dst_ip, uint16_t id, uint16_t seq, uint32_t timeout_ms, ping_result_t* out) {
    int slot = alloc_slot(id, seq);
    if (slot < 0) {
        if (out) {
            out->rtt_ms = 0;
            out->status = PING_UNKNOWN_ERROR;
            out->icmp_type = 0xFF;
            out->icmp_code = 0xFF;
        }
        return false;
    }

    icmp_send_echo(dst_ip, id, seq, NULL);

    uint32_t waited = 0;
    while(waited < timeout_ms){
        if(g_pending[slot].received){
            if (out) {
                out->icmp_type = g_pending[slot].rx_type;
                out->icmp_code = g_pending[slot].rx_code;
                out->status = (uint8_t)map_status(g_pending[slot].rx_type, g_pending[slot].rx_code);
                if (g_pending[slot].end_ms >= g_pending[slot].start_ms) out->rtt_ms = g_pending[slot].end_ms - g_pending[slot].start_ms;
                else out->rtt_ms = 0;
            }
            bool ok = (g_pending[slot].rx_type == ICMP_ECHO_REPLY);
            free_slot(slot);
            return ok;
        }
        sleep(POLL_MS);
        waited += POLL_MS;
    }

    if (out) {
        out->rtt_ms = 0;
        out->status = PING_TIMEOUT;
        out->icmp_type = 0xFF;
        out->icmp_code = 0xFF;
    }
    free_slot(slot);
    return false;
}
