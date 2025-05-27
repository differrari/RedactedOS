#include "xHCIKeyboard.hpp"
#include "xhci.h"
#include "input_dispatch.h"
#include "memory/page_allocator.h"

void xHCIKeyboard::request_data(){
    requesting = true;
    latest_ring = &device->endpoint_transfer_ring[device->endpoint_transfer_index++];
            
    if (device->input_buffer == 0x0){
        uint64_t buffer_addr = (uint64_t)alloc_page(device->poll_packetSize, true, true, true);
        device->input_buffer = (uint8_t*)buffer_addr;
    }
    
    latest_ring->parameter = (uintptr_t)device->input_buffer;
    latest_ring->status = device->poll_packetSize;
    latest_ring->control = (TRB_TYPE_NORMAL << 10) | (1 << 5) | device->endpoint_transfer_cycle_bit;

    if (device->endpoint_transfer_index == MAX_TRB_AMOUNT - 1){
        make_ring_link_control(device->endpoint_transfer_ring, device->endpoint_transfer_cycle_bit);
        device->endpoint_transfer_cycle_bit = !device->endpoint_transfer_cycle_bit;
        device->endpoint_transfer_index = 0;
    }

    ring_doorbell(device->slot_id, device->poll_endpoint);
}

void xHCIKeyboard::process_data(){
    if (!requesting){
        return;
    }
    
    keypress kp = {0};
    if (!xhci_await_response((uintptr_t)latest_ring,TRB_TYPE_TRANSFER))
        xhci_sync_events();//TODO: we're just consuming the event without even looking to see if it's the right one, this is wrong, seriously, IRQ await would fix this

    keypress *rkp = (keypress*)device->input_buffer;
    if (is_new_keypress(rkp, &last_keypress) || repeated_keypresses > 3){
        if (is_new_keypress(rkp, &last_keypress))
            repeated_keypresses = 0;
        kp.modifier = rkp->modifier;
        // kprintf_raw("Mod: %i", kp.modifier);
        for (int i = 0; i < 6; i++){
            kp.keys[i] = rkp->keys[i];
            // kprintf_raw("Key [%i]: %i", i, kp.keys[i]);
        }
        last_keypress = kp;
    } else
        repeated_keypresses++;

    xhci_kbd_request_data();

    register_keypress(kp);
}