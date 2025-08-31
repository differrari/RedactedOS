#pragma once

#include "types.h"
#include "dev/driver_base.h"
#include "process/process.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_IRQ 34

typedef enum WAVE_TYPE {
    WAVE_SQUARE
} WAVE_TYPE;

bool init_audio();
void audio_handle_interrupt();

sizedptr audio_request_buffer(uint32_t device);
void audio_submit_buffer();//TODO: this should be automatic

process_t* init_audio_mixer();

extern driver_module audio_module;

#ifdef __cplusplus
}
#endif