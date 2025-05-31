#include "xHCIKeyboard.hpp"
#include "xhci.h"
#include "input_dispatch.h"
#include "console/kio.h"
#include "memory/page_allocator.h"

void xHCIKeyboard::request_data(){
    requesting = true;
    latest_ring = &endpoint->endpoint_transfer_ring[endpoint->endpoint_transfer_index++];
            
    if (endpoint->input_buffer == 0x0){
        uint64_t buffer_addr = (uint64_t)alloc_page(endpoint->poll_packetSize, true, true, true);
        endpoint->input_buffer = (uint8_t*)buffer_addr;
    }
    
    latest_ring->parameter = (uintptr_t)endpoint->input_buffer;
    latest_ring->status = endpoint->poll_packetSize;
    latest_ring->control = (TRB_TYPE_NORMAL << 10) | (1 << 5) | endpoint->endpoint_transfer_cycle_bit;

    if (endpoint->endpoint_transfer_index == MAX_TRB_AMOUNT - 1){
        make_ring_link_control(endpoint->endpoint_transfer_ring, endpoint->endpoint_transfer_cycle_bit);
        endpoint->endpoint_transfer_cycle_bit = !endpoint->endpoint_transfer_cycle_bit;
        endpoint->endpoint_transfer_index = 0;
    }

    ring_doorbell(slot_id, endpoint->poll_endpoint);
}

void xHCIKeyboard::process_data(){
    if (!requesting){
        return;
    }
    
    keypress kp;
    if (!xhci_await_response((uintptr_t)latest_ring,TRB_TYPE_TRANSFER))
        xhci_sync_events();//TODO: we're just consuming the event without even looking to see if it's the right one, this is wrong, seriously, IRQ await would fix this

    keypress *rkp = (keypress*)endpoint->input_buffer;
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
        register_keypress(kp);
    } else
        repeated_keypresses++;

    request_data();

}