#pragma once

#include "types.h"
#include "dev/driver_base.h"
#include "process/process.h"

#define AUDIO_DRIVER_BUFFER_SIZE    256
// TODO: static_assert((AUDIO_DRIVER_BUFFER_SIZE & 0x01) == 0x00, "Audio buffer size must be even.");

#ifdef __cplusplus
extern "C" {
#endif

bool init_audio();

sizedptr audio_request_buffer(uint32_t device);
void audio_submit_buffer();//TODO: this should be automatic

process_t* init_audio_mixer();

extern driver_module audio_module;

#ifdef __cplusplus
}
#endif