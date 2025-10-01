#pragma once

#include "types.h"

typedef struct audio_samples {
    sizedptr samples;
    uint32_t smpls_per_channel;
    uint8_t  channels;
    float    secs;
} audio_samples;

typedef struct mixer_buf {
    int16_t* samples;
    size_t   sample_count;
    uint32_t left_level;
    uint32_t right_level;
} mixer_buf;

typedef struct mixer_input {
    mixer_buf buf[2];
    uint8_t   channels;
} mixer_input;

typedef enum MIXER_CMND {
    MIXER_SETLEVEL,
    MIXER_MUTE,
    MIXER_UNMUTE,
    MIXER_CLOSE_LINE,   // TODO: eventually done by audio_close()
} MIXER_CMND;

typedef struct mixer_command {
    uint32_t command;
    int64_t  value;
} mixer_command;

typedef enum WAVE_TYPE {
    WAVE_SQUARE,
    WAVE_TRIG,
    WAVE_SAW,
} WAVE_TYPE;

#define PHASE_MASK 0x00FFFFFF
#define PHASE_MAX  PHASE_MASK
#define PHASE_MID  (PHASE_MAX >> 1)

#define WAVE_MID_VALUE  0x80000000

#define AUDIO_LEVEL_MAX UINT32_MAX


#ifdef __cplusplus
extern "C" {
#endif
float sample_raw_wave(WAVE_TYPE type, uint32_t phase);
uint32_t sample_wave(WAVE_TYPE type, uint32_t phase, uint32_t amplitude);

bool play_audio_sync(audio_samples *audio, uint32_t amplitude);
bool play_audio_async(audio_samples *audio, uint32_t amplitude, file *mixin);
bool play_completed(file *mixin);

#ifdef __cplusplus
}
#endif