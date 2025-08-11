#include "USBKeyboard.hpp"
#include "input_dispatch.h"
#include "console/kio.h"
#include "memory/page_allocator.h"
#include "usb.hpp"

void USBKeyboard::request_data(USBDriver *driver){
    requesting = true;

    if (buffer == 0){
        buffer = palloc(packet_size, true, true, true);
    }

    if (!driver->poll(slot_id, endpoint, buffer, packet_size))
        return;

    if (!driver->use_interrupts){
        process_keypress((keypress*)buffer);
        return;
    }
}

void USBKeyboard::process_data(USBDriver *driver){
    if (!requesting)
        return;
    
    process_keypress((keypress*)buffer);

    if (driver->use_interrupts)
        request_data(driver);
}

void USBKeyboard::process_keypress(keypress *rkp){
    keypress kp;
    if (is_new_keypress(rkp, &last_keypress) || repeated_keypresses > 3){
        if (is_new_keypress(rkp, &last_keypress)){
            repeated_keypresses = 0;
            remove_double_keypresses(rkp, &last_keypress);
        }
        kp.modifier = rkp->modifier;
        // kprintf("Mod: %i", kp.modifier);
        for (int i = 0; i < 6; i++){
            kp.keys[i] = rkp->keys[i];
            // kprintf("Key [%i]: %x", i, kp.keys[i]);
        }
        last_keypress = kp;
        register_keypress(kp);
    } else
        repeated_keypresses++;

    requesting = false;
}