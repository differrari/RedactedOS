
#include "types.h"
#include "syscalls/syscalls.h"
#include "math/math.h"
#include "cuatro.h"
#include "tone.h"


#define PHASE_MASK 0x00FFFFFF
#define PHASE_MAX  PHASE_MASK
#define PHASE_MID  (PHASE_MAX >> 1)


static int16_t wave_sample(WAVE_TYPE type, uint32_t phase, rng_t* rng){
    float wave = 0;
    switch (type) {
        case WAVE_SILENCE:
            break;
        case WAVE_TRIG: {
            float fase = 0.25 + (float)phase / (float)PHASE_MAX;
            wave = 2*(absf(fase-floor(fase + 0.5f))) - 0.5;
            break;
        }
        case WAVE_SAW:
            wave = 0.5 - (float)phase / (float)PHASE_MAX;
            break;
        case WAVE_SQUARE:
            wave = (phase < PHASE_MID) ? 0.5 : -0.5;
            break;
        case WAVE_NOISE: {
            return (int16_t)rng_next16(rng);
        }
    }
    return (int16_t)(wave * UINT16_MAX);
}

static inline float ratio_to_phase(uint16_t ratio){
    ratio = min(90, max(10, ratio));
    float result = ((float)PHASE_MAX * ratio) / 100.f;
    return result;
}

static void wave_generate(sound_defn* sound, int16_t* sample, size_t count){
    float freq = sound->start_freq;
    float freq_delta = (sound->end_freq - sound->start_freq) / count;
    uint32_t phase = 0;
    rng_t rng;
    rng_init_random(&rng);
    while (count--){
        uint32_t phase_incr = (uint32_t)(freq * PHASE_MAX / 44100.f);
        *sample++ = wave_sample(sound->waveform, phase, &rng);
        phase = (phase + phase_incr) & PHASE_MASK;
        freq += freq_delta;
    }
}

void sound_create(float duration, float delay, sound_defn* sound, audio_samples* audio){
    audio->channels = 1;    // mono only
    audio->secs = duration + delay;
    audio->smpls_per_channel = (duration + delay) * 44100.f;
    audio->samples.size = audio->smpls_per_channel;
    audio->samples.ptr = (uintptr_t)malloc(audio->samples.size * sizeof(int16_t));
    size_t delay_samples = delay * 44100.f;
    wave_generate(sound, ((int16_t*)audio->samples.ptr) + delay_samples, audio->samples.size - delay_samples);
}

void sound_shape(envelope_defn* env, audio_samples* audio){
    if (env->shape != ENV_NONE){
        size_t attack = min(audio->samples.size, max(0, (int)(audio->samples.size * env->attack)));
        size_t sustain = min(audio->samples.size, max(0, (int)(audio->samples.size * env->sustain)));
        size_t decay = audio->samples.size - attack - sustain;
        int16_t* sample = (int16_t*)audio->samples.ptr;
        float delta = (float)INT16_MAX / attack;
        float level = 0;
        while (attack--){
            *sample = (int64_t)(*sample * level * level) / (INT16_MAX * INT16_MAX);
            level += delta;
            ++sample;
        }
        sample += sustain;
        delta = (float)INT16_MAX / decay;
        level = INT16_MAX;
        while (decay--){
            *sample = (int64_t)(*sample * level * level) / (INT16_MAX * INT16_MAX);
            level -= delta;
            ++sample;
        }
    }
}





// float sample_raw_wave(WAVE_TYPE type, uint32_t phase){
//     switch (type) {
//         case WAVE_TRIG: {
//             float t = ((float)phase/(float)PHASE_MAX);
//             float trig = 2*(absf(t-floor(t + 0.5f)));
//             return trig;
//         }
//         case WAVE_SAW:
//             return (float)(PHASE_MAX - phase) / (float)PHASE_MAX;
//         case WAVE_SQUARE:
//             return (phase < PHASE_MID) ? 0.f : 1.f;
//     }
//     return 0;
// }

// uint32_t sample_wave(WAVE_TYPE type, uint32_t phase, int16_t amplitude){
//     return sample_raw_wave(type, phase) * amplitude;
// }

