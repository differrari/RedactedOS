#include "port_manager.h"
#include "types.h"
#include "networking/port_manager.h"
#include "net/internet_layer/ipv4.h"

static port_entry_t g_port_table[PROTO_COUNT][MAX_PORTS];//tab proto/port

static inline bool port_valid(uint16_t p) {
    return p > 0 && p < MAX_PORTS;
}
static inline bool proto_valid(protocol_t proto) {
    return (uint32_t)proto< PROTO_COUNT;
}

void port_manager_init() {
    for (int pr = 0; pr < PROTO_COUNT; ++pr) {
        for (uint32_t p = 0; p < MAX_PORTS; ++p) {
            g_port_table[pr][p].used    = false;
            g_port_table[pr][p].pid     = PORT_FREE_OWNER;
            g_port_table[pr][p].handler = NULL;
        }
    }
}

int port_alloc_ephemeral(protocol_t proto,
                         uint16_t pid,
                         port_recv_handler_t handler)
{
    if (!proto_valid(proto)) return -1;
    for (uint16_t p = PORT_MIN_EPHEMERAL; p <= PORT_MAX_EPHEMERAL; ++p) {
        port_entry_t *e = &g_port_table[proto][p];
        if (!e->used) {
            e->used = true;
            e->pid = pid;
            e->handler = handler;
            return (int)p;
        }
    }
    return -1;
}

bool port_bind_manual(protocol_t proto,
                      uint16_t port,
                      uint16_t pid,
                      port_recv_handler_t handler)
{
    if (!proto_valid(proto) || !port_valid(port)) return false;
    port_entry_t *e = &g_port_table[proto][port];
    if (e->used) return false;
    e->used = true;
    e->pid = pid;
    e->handler = handler;
    return true;
}

bool port_unbind(protocol_t proto,
                 uint16_t port,
                 uint16_t pid)
{
    if (!proto_valid(proto) || !port_valid(port)) return false;
    port_entry_t *e = &g_port_table[proto][port];
    if (!e->used || e->pid != pid) return false;
    e->used = false;
    e->pid = PORT_FREE_OWNER;
    e->handler = NULL;
    return true;
}

void port_unbind_all(uint16_t pid) {
    for (int pr = 0; pr < PROTO_COUNT; ++pr) {
        for (uint16_t p = 1; p < MAX_PORTS; ++p) {
            port_entry_t *e = &g_port_table[pr][p];
            if (e->used && e->pid == pid) {
                e->used    = false;
                e->pid     = PORT_FREE_OWNER;
                e->handler = NULL;
            }
        }
    }
}

bool port_is_bound(protocol_t proto, uint16_t port) {
    if (!proto_valid(proto) || !port_valid(port)) return false;
    return g_port_table[proto][port].used;
}

uint16_t port_owner_of(protocol_t proto, uint16_t port) {
    if (!proto_valid(proto) || !port_valid(port)) return PORT_FREE_OWNER;
    return g_port_table[proto][port].pid;
}

port_recv_handler_t port_get_handler(protocol_t proto, uint16_t port) {
    if (!proto_valid(proto) || !port_valid(port)) return NULL;
    return g_port_table[proto][port].used
        ? g_port_table[proto][port].handler
        : NULL;
}
