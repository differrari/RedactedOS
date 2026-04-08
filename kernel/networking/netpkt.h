#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct netpkt netpkt_t;

typedef void (*netpkt_free_fn)(void* ctx, uintptr_t base, uint32_t alloc_size);

#define NETPKT_MAX_ALLOC 65536u
#define NETPKT_MAX_PAGE_BYTES (32ull * 1024ull * 1024ull)

netpkt_t* netpkt_alloc(uint32_t data_capacity, uint32_t headroom, uint32_t tailroom);
netpkt_t* netpkt_wrap(uintptr_t base, uint32_t alloc_size, uint32_t data_off, uint32_t data_len, netpkt_free_fn free_fn, void* ctx);
netpkt_t* netpkt_view(netpkt_t* parent, uint32_t off, uint32_t len);

void netpkt_ref(netpkt_t* p);
void netpkt_unref(netpkt_t* p);

uintptr_t netpkt_data(const netpkt_t* p);
uint32_t netpkt_len(const netpkt_t* p);

uint32_t netpkt_headroom(const netpkt_t* p);
uint32_t netpkt_tailroom(const netpkt_t* p);

bool netpkt_pull(netpkt_t* p, uint32_t bytes);
bool netpkt_trim(netpkt_t* p, uint32_t new_len);

void* netpkt_push(netpkt_t* p, uint32_t bytes);
void* netpkt_put(netpkt_t* p, uint32_t bytes);

bool netpkt_ensure_headroom(netpkt_t* p, uint32_t need);
bool netpkt_ensure_tailroom(netpkt_t* p, uint32_t need);

#ifdef __cplusplus
}
#endif
