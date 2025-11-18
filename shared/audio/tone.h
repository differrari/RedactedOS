#pragma once

#include "cuatro.h"


typedef enum WAVE_TYPE {
    WAVE_SILENCE,
    WAVE_SQUARE,
    WAVE_TRIG,
    WAVE_SAW,
    WAVE_NOISE,
} WAVE_TYPE;

typedef struct sound_defn {
    WAVE_TYPE waveform;
    float     start_freq;
    float     end_freq;
} sound_defn;

typedef enum ENV_TYPE {
    ENV_NONE,
    ENV_ASD,
    // ENV_ADSR,
} ENV_TYPE;

typedef struct envelope_defn {
    ENV_TYPE  shape;
    float     attack;  // 0..1 as fraction of tone duration
    float     sustain; // ditto
} envelope_defn;


#ifdef __cplusplus
extern "C" {
#endif

void sound_create(float seconds, sound_defn *sound, audio_samples *audio);
void sound_shape(envelope_defn* env, audio_samples* audio);

#ifdef __cplusplus
}
#endif