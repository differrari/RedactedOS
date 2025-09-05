#pragma once

#include "types.h"

typedef enum WAVE_TYPE {
    WAVE_SQUARE,
    WAVE_TRIG,
    WAVE_SAW,
} WAVE_TYPE;

#ifdef __cplusplus
extern "C" {
#endif
float sample_raw_wave(WAVE_TYPE type, uint32_t accumulator, uint32_t period);
uint32_t sample_wave(WAVE_TYPE type, uint32_t accumulator, uint32_t period, uint32_t amplitude);
// void make_wave(WAVE_TYPE type, float freq, float seconds);
#ifdef __cplusplus
}
#endif