#ifndef _POWER_H_
#define _POWER_H_

#include "types.h"

/**
 * @brief Powers off the system
 * 
 * This function attempts to power off the system using the mailbox interface.
 * If that fails, it falls back to a hardware-specific method.
 */
void power_off();

/**
 * @brief Reboots the system
 * 
 * This function attempts to reboot the system using the mailbox interface.
 * If that fails, it falls back to a hardware-specific method.
 */
void reboot();

#endif // _POWER_H_
