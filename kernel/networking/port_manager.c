#include "networking/port_manager.h"
#include "types.h"
#include "random/random.h"
#include "net/network_types.h"

static inline bool proto_valid(protocol_t proto) {
    return (uint32_t)proto < PROTO_COUNT;
}

void port_manager_init(port_manager_t* pm) {
    if (!pm) return;
    for (uint32_t pr = 0; pr < PROTO_COUNT; ++pr) {
        for (uint32_t p = 0; p < MAX_PORTS; ++p) {
            pm->tab[pr][p].used    = false;
            pm->tab[pr][p].pid     = PORT_FREE_OWNER;
            pm->tab[pr][p].handler = NULL;
        }
    }
}

int port_alloc_ephemeral(port_manager_t* pm,
                         protocol_t proto,
                         uint16_t pid,
                         port_recv_handler_t handler)
{
    if (!pm || !proto_valid(proto)) return -1;

    rng_t rng;
    rng_init_random(&rng);
    uint32_t seed = rng_next32(&rng);

    const uint32_t minp = (uint32_t)PORT_MIN_EPHEMERAL;
    const uint32_t maxp = (uint32_t)PORT_MAX_EPHEMERAL;
    const uint32_t range = maxp - minp + 1u;
    const uint32_t first = minp + (seed % range);

    for (uint32_t i = 0; i < range; ++i) {
        uint32_t p = minp + ((first - minp + i) % range);
        port_entry_t *e = &pm->tab[proto][p];
        if (!e->used) {
            e->used = true;
            e->pid = pid;
            e->handler = handler;
            return (int)p;
        }
    }
    return -1;
}

bool port_bind_manual(port_manager_t* pm,
                      protocol_t proto,
                      uint16_t port,
                      uint16_t pid,
                      port_recv_handler_t handler)
{
    if (!pm || !proto_valid(proto)) return false;
    port_entry_t *e = &pm->tab[proto][port];
    if (e->used) return false;
    e->used = true;
    e->pid = pid;
    e->handler = handler;
    return true;
}

bool port_unbind(port_manager_t* pm,
                 protocol_t proto,
                 uint16_t port,
                 uint16_t pid)
{
    if (!pm || !proto_valid(proto)) return false;
    port_entry_t *e = &pm->tab[proto][port];
    if (!e->used || e->pid != pid) return false;
    e->used = false;
    e->pid = PORT_FREE_OWNER;
    e->handler = NULL;
    return true;
}

void port_unbind_all(port_manager_t* pm, uint16_t pid) {
    if (!pm) return;
    for (uint32_t pr = 0; pr < PROTO_COUNT; ++pr) {
        for (uint32_t p = 1; p < MAX_PORTS; ++p) {
            port_entry_t *e = &pm->tab[pr][p];
            if (e->used && e->pid == pid) {
                e->used    = false;
                e->pid     = PORT_FREE_OWNER;
                e->handler = NULL;
            }
        }
    }
}

bool port_is_bound(const port_manager_t* pm, protocol_t proto, uint16_t port) {
    if (!pm || !proto_valid(proto)) return false;
    return pm->tab[proto][port].used;
}

uint16_t port_owner_of(const port_manager_t* pm, protocol_t proto, uint16_t port) {
    if (!pm || !proto_valid(proto)) return PORT_FREE_OWNER;
    return pm->tab[proto][port].pid;
}

port_recv_handler_t port_get_handler(const port_manager_t* pm, protocol_t proto, uint16_t port) {
    if (!pm || !proto_valid(proto)) return NULL;
    const port_entry_t* e = &pm->tab[proto][port];
    return e->used ? e->handler : NULL;
}
