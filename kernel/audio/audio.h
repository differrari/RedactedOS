#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_IRQ 34

bool init_audio();
void audio_handle_interrupt();

#ifdef __cplusplus
}
#endif