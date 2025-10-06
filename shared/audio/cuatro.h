#pragma once

#include "types.h"

typedef struct audio_samples {
    sizedptr samples;
    uint32_t smpls_per_channel;
    uint8_t  channels;
    int16_t  amplitude;
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
    int8_t   lineId;
    uint32_t command;
    union {
        intptr_t value;
        audio_samples *audio;
    };
} mixer_command;

typedef struct mixer_line_data {
    int8_t   lineId;
    size_t   count[2];
} mixer_line_data;


#define AUDIO_LEVEL_MAX INT16_MAX
#define MIXER_INPUTS 4

#ifdef __cplusplus
extern "C" {
#endif

bool play_audio_sync(audio_samples *audio, int16_t amplitude);
int8_t play_audio_async(audio_samples *audio, int16_t amplitude);

int8_t mixer_open_line();
void mixer_close_line(int8_t line);
bool mixer_still_playing(int8_t line);
void mixer_play_async(int8_t line, audio_samples* audio);
bool mixer_mute();
bool mixer_unmute();
uint32_t mixer_set_level(int16_t level);

#ifdef __cplusplus
}
#endif