#include "USBKeyboard.hpp"
#include "input/input_dispatch.h"
#include "memory/page_allocator.h"
#include "usb.hpp"
#include "async.h"
#include "exceptions/timer.h"

static uint8_t held[256];
static uint64_t next_repeat[256];

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

void USBKeyboard::process_keypress(keypress *rkp){
    bool handled_key = false;

    if (is_new_keypress(rkp, &last_keypress)) {
        keypress kp = {};
        kp.modifier = rkp->modifier;
        // kprintf("Mod: %i", kp.modifier);
        for (int i = 0; i < 6; i++){
            kp.keys[i] = rkp->keys[i];
            // if (i == 0) kprintf("Key [%i]: %x", i, kp.keys[i]);
        }
        handled_key = register_keypress(kp);
    }
    if (!handled_key){
        uint64_t now = get_time();

        for (int i = 0; i < 8; i++){
            char oldkey = (char)(last_keypress.modifier & (1 << i));
            char newkey = (char)(rkp->modifier & (1 << i));
            if (oldkey == newkey) continue;

            kbd_event event = {};
            event.type = oldkey ? MOD_RELEASE : MOD_PRESS;
            event.modifier = oldkey ? oldkey : newkey;
            register_event(event);
        }
        for (int i = 0; i < 6; i++) {
            char key = (char)last_keypress.keys[i];
            if (!key) continue;
            bool present = false;
            for (int j = 0; j < 6; j++)
                if ((char)rkp->keys[j] == key) {
                    present = true;
                    break;
                }
            if (present) continue;

            kbd_event event = {};
            event.type = KEY_RELEASE;
            event.key = key;
            register_event(event);

            held[(uint8_t)key] = 0;
            next_repeat[(uint8_t)key] = 0;
        }

        for (int i = 0; i < 6; i++) {
            uint8_t key = (uint8_t)rkp->keys[i];
            if (!key) continue;

            bool present = false;
            for (int j = 0; j < 6; j++)
                if ((uint8_t)last_keypress.keys[j] == key) {
                    present = true;
                    break;
                }

            if (!present) {
                kbd_event event = {};
                event.type = KEY_PRESS;
                event.key = (char)key;
                register_event(event);

                held[key] = 1;
                next_repeat[key] = now + 500;
                continue;
            }

            if (!held[key]) {
                held[key] = 1;
                next_repeat[key] = now + 500;
            }

            if (now < next_repeat[key]) continue;

            kbd_event event = {};
            event.type = KEY_PRESS;
            event.key = (char)key;
            register_event(event);

            next_repeat[key] = now + 33;
        }
    }
    memcpy(&last_keypress, rkp, sizeof(keypress));

    requesting = false;
}