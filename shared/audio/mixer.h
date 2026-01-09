#pragma once

typedef enum MIXER_CMND {
    MIXER_SETLEVEL,
    MIXER_MUTE,
    MIXER_UNMUTE,
    MIXER_PLAY,
    MIXER_SETBUFFER,
    MIXER_CLOSE_LINE,
} MIXER_CMND;

typedef struct mixer_command {
    int8_t   lineId;
    uint32_t command;
    audio_samples* audio;
    sizedptr samples;
    //intptr_t value;
    uint32_t delay_ms;
    int16_t level;
    int16_t pan;
    AUDIO_LIFETIME life;
} mixer_command;
