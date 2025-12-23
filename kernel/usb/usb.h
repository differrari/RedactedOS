#pragma once

#include "dev/driver_base.h"

void handle_usb_interrupt();

void init_usb_process();
void usb_start_polling();

extern system_module usb_module;