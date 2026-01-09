#include "dwc2.hpp"
#include "xhci.hpp"
#include "hw/hw.h"
#include "kernel_processes/kprocess_loader.h"

USBDriver *input_driver = 0x0;

bool input_init(){
    #if QEMU
    if (BOARD_TYPE == 2){
    #else
    if (BOARD_TYPE == 2 && RPI_BOARD == 3){
    #endif
        input_driver = new DWC2Driver();
    } else {
        input_driver = new XHCIDriver();
    }
    return input_driver->init();
}

int usb_process_poll(int argc, char* argv[]){
    while (1){
        if (input_driver) input_driver->poll_inputs();
    }
    return 1;
}

void usb_start_polling(){
    if (input_driver) input_driver->poll_inputs();
}

int usb_process_fake_interrupts(int argc, char* argv[]){
    while (1){
        input_driver->handle_interrupt();
    }
    return 1;
}

extern "C" void init_usb_process(){
    if (!input_driver->use_interrupts)
        create_kernel_process("input_poll", &usb_process_poll, 0, 0);
    if (input_driver->quirk_simulate_interrupts)
        create_kernel_process("input_int_mock", &usb_process_fake_interrupts, 0, 0);
}

extern "C" void handle_usb_interrupt(){
    if (input_driver->use_interrupts) input_driver->handle_interrupt();
}

system_module usb_module = (system_module){
    .name = "input",
    .mount = "/in",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = input_init,
    .fini = 0,
    .open = 0,
    .read = 0,
    .write = 0,
    .close = 0,
    .sread = 0,
    .swrite = 0,//TODO implement simple io
    .readdir = 0,
};