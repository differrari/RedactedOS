#include "xhci_kbd.h"
#include "ram_e.h"
#include "console/kio.h"

static const char hid_keycode_to_char[256] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd',
    [0x08] = 'e', [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h',
    [0x0C] = 'i', [0x0D] = 'j', [0x0E] = 'k', [0x0F] = 'l',
    [0x10] = 'm', [0x11] = 'n', [0x12] = 'o', [0x13] = 'p',
    [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x',
    [0x1C] = 'y', [0x1D] = 'z',
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4',
    [0x22] = '5', [0x23] = '6', [0x24] = '7', [0x25] = '8',
    [0x26] = '9', [0x27] = '0',
    [0x28] = '\n', [0x2C] = ' ', [0x2D] = '-', [0x2E] = '=',
    [0x2F] = '[', [0x30] = ']', [0x31] = '\\', [0x33] = ';',
    [0x34] = '\'', [0x35] = '`', [0x36] = ',', [0x37] = '.',
    [0x38] = '/',
};

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

keypress xhci_read_key() {

    keypress kp = {0};
    if (!requesting){
        return kp;
    }
    
    if (!xhci_await_response((uintptr_t)latest_ring,TRB_TYPE_TRANSFER)){
        kprintf("[xHCI error] failed getting key");
        return kp;
    }
    keypress *rkp = (keypress*)default_device->input_buffer;
    kp.modifier = rkp->modifier;
    for (int i = 0; i < 6; i++){
        if (rkp->keys[i] != 0){ kp.keys[i] = hid_keycode_to_char[rkp->keys[i]];
            kprintf("Key [%i]: %c (%i)", kp.keys[i], kp.keys[i]);
        }
    }

    xhci_kbd_request_data();

    return kp;
}