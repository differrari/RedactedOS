#pragma once

#include "types.h"
#include "networking/netpkt.h"

#define NET_FRAGBUF_DEFAULT_MAX_LEN 65535u
#define NET_FRAGBUF_DEFAULT_STEP 2048u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *data;
    uint8_t *blocks;
    uint32_t cap;
    uint32_t total_len;
    uint32_t max_len;
    uint32_t step;
    uint32_t block_count;
    uint8_t have_last;
} net_fragbuf_t;

void net_fragbuf_init(net_fragbuf_t *fb);
void net_fragbuf_free(net_fragbuf_t *fb);
bool net_fragbuf_add(net_fragbuf_t *fb, netpkt_t *pkt, uint32_t pkt_off, uint32_t frag_off, uint32_t frag_len, uint8_t more);
bool net_fragbuf_complete(const net_fragbuf_t *fb);
netpkt_t* net_fragbuf_take_packet(net_fragbuf_t *fb);

#ifdef __cplusplus
}
#endif
