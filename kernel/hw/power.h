#pragma once

typedef enum {
    SHUTDOWN_REBOOT = 0,
    SHUTDOWN_POWEROFF = 1,
} shutdown_mode;

void hw_shutdown(shutdown_mode mode);