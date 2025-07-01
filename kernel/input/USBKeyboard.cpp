#include "USBKeyboard.hpp"
#include "input_dispatch.h"
#include "console/kio.h"
#include "memory/page_allocator.h"
#include "usb.hpp"

void USBKeyboard::request_data(USBDriver *driver){
    requesting = true;

    if (buffer == 0){
        buffer = alloc_page(packet_size, true, true, true);
    }
    
    if (driver->poll(slot_id, endpoint, buffer, packet_size) && !driver->use_interrupts){
        process_keypress((keypress*)buffer);
    }
}

void USBKeyboard::process_data(USBDriver *driver){
    if (!requesting){
        return;
    }
    
    // if (!xhci_await_response((uintptr_t)latest_ring,TRB_TYPE_TRANSFER))
    //     xhci_sync_events();//TODO: we're just consuming the event without even looking to see if it's the right one, this is wrong, seriously, IRQ await would fix this

    // keypress *rkp = (keypress*)endpoint->input_buffer;
    // process_keypress(rkp);

    // request_data();

}

void USBKeyboard::process_keypress(keypress *rkp){
    keypress kp;
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
}