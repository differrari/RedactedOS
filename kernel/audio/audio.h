#pragma once

#include "types.h"
#include "dev/driver_base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_IRQ 34

bool init_audio();
void audio_handle_interrupt();

extern driver_module audio_module;

#ifdef __cplusplus
}
#endif