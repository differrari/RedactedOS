#pragma once

#include "types.h"
#include "xhci_types.h"

void xhci_enable_verbose();
bool xhci_input_init();

void make_ring_link_control(trb* ring, bool cycle);
void ring_doorbell(uint32_t slot, uint32_t endpoint);
bool xhci_await_response(uint64_t command, uint32_t type);

void xhci_handle_interrupt();