#include "types.h"
#include "syscalls/syscalls.h"
#include "math/math.h"
#include "cuatro.h"
#include "tone.h"


#define PHASE_MASK 0x00FFFFFF
#define PHASE_MAX  PHASE_MASK
#define PHASE_MID  (PHASE_MAX >> 1)


static audio_sample_t wave_sample(WAVE_TYPE type, uint32_t phase, rng_t* rng){
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
            return (audio_sample_t)rng_next16(rng);
        }
    }
    return (audio_sample_t)(wave * UINT16_MAX);
}

static void wave_generate(sound_defn* sound, audio_sample_t* sample, size_t count){
    float freq = sound->start_freq;
    float freq_delta = (sound->end_freq - sound->start_freq) / count;
    uint32_t phase = 0;
    rng_t rng;
    // TODO: rng_init_random(&rng);
    rng.s0 = get_time();
    rng.s1 = (uint64_t)&wave_generate ^ rng.s0;
    while (count--){
        uint32_t phase_incr = (uint32_t)(freq * PHASE_MAX / 44100.f);
        *sample++ = wave_sample(sound->waveform, phase, &rng);
        phase = (phase + phase_incr) & PHASE_MASK;
        freq += freq_delta;
    }
}

void sound_create(float duration, sound_defn* sound, audio_samples* audio){
    // !! MONO only
    audio->channels = 1;
    audio->smpls_per_channel = duration * 44100.f;
    audio->samples.size = audio->smpls_per_channel * sizeof(audio_sample_t);
    audio->samples.ptr = (uintptr_t)malloc(audio->samples.size);
    wave_generate(sound, (audio_sample_t*)audio->samples.ptr, audio->smpls_per_channel);
}

void sound_shape(envelope_defn* env, audio_samples* audio){
    // !! MONO only
    if (env->shape != ENV_NONE && audio->channels == 1){
        size_t attack = 1 + min(audio->smpls_per_channel, max(0, (int)(audio->smpls_per_channel * env->attack)));
        size_t sustain = min(audio->smpls_per_channel - attack, max(0, (int)(audio->smpls_per_channel * env->sustain)));
        size_t decay = max(0, (int)audio->smpls_per_channel - (int)attack - (int)sustain);
        audio_sample_t* sample = (audio_sample_t*)audio->samples.ptr;
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