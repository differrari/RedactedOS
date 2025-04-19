#pragma once 

#include "types.h"

void vgp_flush();
void vgp_transfer_to_host();
void vgp_clear(uint32_t color);
bool vgp_init();