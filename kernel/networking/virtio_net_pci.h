#pragma once

#include "types.h"

#define NET_IRQ 32

bool vnp_find_network();
void vnp_handle_interrupt();
void vnp_enable_verbose();