#pragma once

#include "types.h"

typedef struct audio_samples {
    sizedptr samples;
    uint32_t smpls_per_channel;
    uint8_t  channels;
    uint32_t amplitude;
    float    secs;
} audio_samples;

typedef enum MIXER_CMND {
    MIXER_SETLEVEL,
    MIXER_MUTE,
    MIXER_UNMUTE,
    MIXER_PLAY,
    MIXER_CLOSE_LINE,
} MIXER_CMND;

typedef struct mixer_command {
    intptr_t line;
    uint32_t command;
    union {
        intptr_t value;
        audio_samples *audio;
    };
} mixer_command;

typedef struct mixer_line_data {
    intptr_t line;
    intptr_t buf[2];
} mixer_line_data;

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
intptr_t play_audio_async(audio_samples *audio, uint32_t amplitude);

intptr_t mixer_open_line();
void mixer_close_line(intptr_t line);
bool mixer_still_playing(intptr_t line);
void mixer_play_async(intptr_t line, audio_samples* audio);

#ifdef __cplusplus
}
#endif