#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

#define NET_IRQ 32

bool network_init();
void network_handle_interrupt();
bool network_bind_port(uint16_t port, uint16_t process);
bool network_unbindbind_port(uint16_t port, uint16_t process);

#ifdef __cplusplus
}
#endif