#include "xhci_kbd.h"
#include "ram_e.h"
#include "console/kio.h"
#include "input_dispatch.h"

xhci_usb_device* default_device;

void set_default_kbd(xhci_usb_device* kbd){
    default_device = kbd;
}

trb* latest_ring;
bool requesting = false;

void xhci_kbd_request_data() {
    requesting = true;
    latest_ring = &default_device->endpoint_transfer_ring[default_device->endpoint_transfer_index++];
            
    if (default_device->input_buffer == 0x0){
        uint64_t buffer_addr = (uint64_t)alloc_dma_region(default_device->poll_packetSize);
        default_device->input_buffer = (uint8_t*)buffer_addr;
    }
    
    latest_ring->parameter = (uintptr_t)default_device->input_buffer;
    latest_ring->status = default_device->poll_packetSize;
    latest_ring->control = (TRB_TYPE_NORMAL << 10) | (1 << 5) | default_device->endpoint_transfer_cycle_bit;

    if (default_device->endpoint_transfer_index == MAX_TRB_AMOUNT - 1){
        make_ring_link_control(default_device->endpoint_transfer_ring, default_device->endpoint_transfer_cycle_bit);
        default_device->endpoint_transfer_cycle_bit = !default_device->endpoint_transfer_cycle_bit;
        default_device->endpoint_transfer_index = 0;
    }

    ring_doorbell(default_device->slot_id, default_device->poll_endpoint);

}

bool is_new_keypress(keypress* current, keypress* previous) {
    if (current->modifier != previous->modifier) return true;

    for (int i = 0; i < 6; i++)
        if (current->keys[i] != previous->keys[i]) return true;

    return false;
}

keypress last_keypress;

int repeated_keypresses = 0; 

keypress xhci_read_key() {

    keypress kp = {0};
    if (!requesting){
        return kp;
    }
    
    if (!xhci_await_response((uintptr_t)latest_ring,TRB_TYPE_TRANSFER))
        xhci_sync_events();//TODO: we're just consuming the event without even looking to see if it's the right one, this is wrong, seriously, IRQ await would fix this

    keypress *rkp = (keypress*)default_device->input_buffer;
    if (is_new_keypress(rkp, &last_keypress) || repeated_keypresses > 3){
        if (is_new_keypress(rkp, &last_keypress))
            repeated_keypresses = 0;
        kp.modifier = rkp->modifier;
        kprintf_raw("Mod: %i", kp.modifier);
        for (int i = 0; i < 6; i++){
            kp.keys[i] = rkp->keys[i];
            kprintf_raw("Key [%i]: %i", i, kp.keys[i]);
        }
        last_keypress = kp;
    } else
        repeated_keypresses++;

    xhci_kbd_request_data();

    register_keypress(kp);

    return kp;
}