#pragma once

#ifdef __cplusplus
extern "C" {
#endif 

#include "xhci.h"
#include "xhci_types.h"

#include "keypress.h"

void xhci_bridge_request_data(uint8_t slot_id, uint8_t endpoint_id);
void xhci_process_input(uint8_t slot_id, uint8_t endpoint_id);

void xhci_initialize_manager(uint32_t capacity);
void xhci_configure_device(uint8_t slot_id, uint8_t endpoint_id, xhci_usb_device *device);

#ifdef __cplusplus
}
#endif 