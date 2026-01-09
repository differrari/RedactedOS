#include "USBMouse.hpp"
#include "input/input_dispatch.h"
#include "memory/page_allocator.h"
#include "usb.hpp"
#include "async.h"

void USBMouse::request_data(USBDriver *driver){
    requesting = true;

    if (buffer == 0){
        buffer = palloc(packet_size, MEM_PRIV_KERNEL, MEM_RW | MEM_DEV, true);
    }

    if (!driver->poll(slot_id, endpoint, buffer, packet_size))
        return;

    if (!driver->use_interrupts){
        process_mouse_input((mouse_input*)buffer);
        return;
    }
}

void USBMouse::process_data(USBDriver *driver){
    if (!requesting)
        return;
    
    process_mouse_input((mouse_input*)buffer);

    if (driver->use_interrupts){
        delay(interval);
        request_data(driver);
    }
}

void USBMouse::process_mouse_input(mouse_input *rat){
    register_mouse_input(rat);
    requesting = false;
}