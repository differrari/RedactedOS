#include "icmp.h"
#include "net/checksums.h"
#include "std/memory.h"
#include "networking/interface_manager.h"
#include "net/internet_layer/ipv4.h"
#include "net/internet_layer/ipv4_route.h"

extern uintptr_t malloc(uint64_t size);
extern void      free(void *ptr, uint64_t size);
extern void      sleep(uint64_t ms);

#define MAX_PENDING 16
#define POLL_MS 1

typedef struct {
    bool in_use;
    uint16_t id, seq;
    bool received;
} ping_slot_t;

static ping_slot_t g_pending[MAX_PENDING] = {0};

static int  alloc_slot(uint16_t id, uint16_t seq){
    for(int i=0;i<MAX_PENDING;i++){
        if(!g_pending[i].in_use){
            g_pending[i]=(ping_slot_t){true,id,seq,false};
            return i;
        }
    }
    return -1;
}

static void mark_received(uint16_t id,uint16_t seq){
    for(int i=0;i<MAX_PENDING;i++)
        if(g_pending[i].in_use&&g_pending[i].id==id&&g_pending[i].seq==seq)
            g_pending[i].received=true;
}

static void free_slot(int i){
    if(i>=0&&i<MAX_PENDING) g_pending[i].in_use=false;
}

void create_icmp_packet(uintptr_t p,
                        const icmp_data *d)
{
    icmp_packet *pkt = (icmp_packet*)p;
    pkt->type = d->response ? ICMP_ECHO_REPLY : ICMP_ECHO_REQUEST;
    pkt->code = 0;
    pkt->id   = bswap16(d->id);
    pkt->seq  = bswap16(d->seq);

    memset(pkt->payload, 0, sizeof(pkt->payload));
    memcpy(pkt->payload, d->payload, sizeof(pkt->payload));

    pkt->checksum = 0;
}

void icmp_send_echo(uint32_t dst_ip,
                    uint16_t id,
                    uint16_t seq,
                    const uint8_t payload[56])
{
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

void icmp_input(uintptr_t ptr,
                uint32_t  len,
                uint32_t  src_ip,
                uint32_t  dst_ip)
{
    if (len < 8) return;

    icmp_packet *pkt = (icmp_packet*)ptr;
    uint16_t recv_ck = pkt->checksum;
    pkt->checksum = 0;
    if (checksum16((uint16_t*)pkt, len) != recv_ck) return;
    pkt->checksum = recv_ck;

    uint8_t type = pkt->type;
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

        l3_ipv4_interface_t *l3 = l3_ipv4_find_by_ip(dst_ip); // nostro IP
        if (l3 && l3->l2) {
            ipv4_tx_opts_t o = { .index = l3->l3_id, .int_type = 1 };

            char sbuf[16], dbuf[16];
            ipv4_to_string(src_ip, sbuf);
            ipv4_to_string(dst_ip, dbuf);

            ipv4_send_packet(src_ip, 1, (sizedptr){ buf, reply_len }, &o);
        }

        free((void*)buf, reply_len);
        return;
    }

    if(type == ICMP_ECHO_REPLY) {
        mark_received(id, sq);
    }
}


bool icmp_ping(uint32_t dst_ip,
               uint16_t id,
               uint16_t seq,
               uint32_t timeout_ms)
{
    int slot = alloc_slot(id, seq);
    if(slot < 0) return false;

    icmp_send_echo(dst_ip, id, seq, NULL);

    uint32_t waited = 0;
    while(waited < timeout_ms){
        if(g_pending[slot].received){
            free_slot(slot);
            return true;
        }
        sleep(POLL_MS);
        waited += POLL_MS;
    }

    free_slot(slot);
    return false;
}
