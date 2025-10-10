#include "USBKeyboard.hpp"
#include "input_dispatch.h"
#include "console/kio.h"
#include "memory/page_allocator.h"
#include "usb.hpp"
#include "async.h"
#include "exceptions/timer.h"

void USBKeyboard::request_data(USBDriver *driver){
    requesting = true;

    if (buffer == 0){
        buffer = palloc(packet_size, MEM_PRIV_KERNEL, MEM_RW | MEM_DEV, true);
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

    if (driver->use_interrupts){
        delay(interval);
        request_data(driver);
    }
}

uint64_t last_registered[256];

void USBKeyboard::process_keypress(keypress *rkp){
    for (int i = 0; i < 8; i++){
        char oldkey = (last_keypress.modifier & (1 << i));
        char newkey = (rkp->modifier & (1 << i));
        if (oldkey != newkey){
            kbd_event event = {};
            event.type = oldkey ? MOD_RELEASE : MOD_PRESS;
            event.modifier = oldkey ? oldkey : newkey;
            register_event(event);
        }
    }
    for (int i = 0; i < 6; i++) {
        if (rkp->keys[i] != last_keypress.keys[i]){
            if (last_keypress.keys[i]){
                kbd_event event = {};
                event.type = KEY_RELEASE;
                event.key = last_keypress.keys[i];
                last_registered[(uint8_t)event.key] = timer_now_msec();
                register_event(event);
            }
            if (rkp->keys[i]) {
                kbd_event event = {};
                event.type = KEY_PRESS;
                event.key = rkp->keys[i];
                if (timer_now_msec()-last_registered[(uint8_t)event.key] > 50){
                    last_registered[(uint8_t)event.key] = timer_now_msec();
                    register_event(event);
                }
            }
        }
    }
    
    keypress kp = {};
    if (is_new_keypress(rkp, &last_keypress) || repeated_keypresses > 2){
        //TODO: review this code. It's here to prevent qemu's duplicate keyboard input
        if (is_new_keypress(rkp, &last_keypress)){
            repeated_keypresses = 0;
        }
        kp.modifier = rkp->modifier;
        // kprintf("Mod: %i", kp.modifier);
        for (int i = 0; i < 6; i++){
            kp.keys[i] = rkp->keys[i];
            // if (i == 0) kprintf("Key [%i]: %x", i, kp.keys[i]);
        }
        register_keypress(kp);
    } else
        repeated_keypresses++;
    last_keypress = kp;

    requesting = false;
}