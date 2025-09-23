#pragma once

#include "types.h"

typedef enum WAVE_TYPE {
    WAVE_SQUARE,
    WAVE_TRIG,
    WAVE_SAW,
} WAVE_TYPE;

#define PHASE_MASK 0x00FFFFFF
#define PHASE_MAX  PHASE_MASK
#define PHASE_MID  (PHASE_MAX >> 1)

#define WAVE_MID_VALUE 0x80000000

#define AUDIO_LEVEL_MAX UINT32_MAX


#ifdef __cplusplus
extern "C" {
#endif
float sample_raw_wave(WAVE_TYPE type, uint32_t phase);
uint32_t sample_wave(WAVE_TYPE type, uint32_t phase, uint32_t amplitude);
// void make_wave(WAVE_TYPE type, float freq, float seconds);
#ifdef __cplusplus
}
#endif